# Streaming replication with wal_consistency_checking: every iheap and
# dmbtree record is replayed on a standby and the result compared with the
# primary's page image.  Queries on the standby see the same data.  (The
# tables' secondary indexes are dmbtree indexes, iheap.default_index_am,
# so the workload's key changes are in-place updates.)
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IheapTest;

# 'iheap' and 'dmbtree' are custom resource managers: named explicitly
# ("all" is expanded before shared_preload_libraries are loaded)
my $primary = iheap_node('primary', allows_streaming => 1,
	extra_conf => "wal_consistency_checking = 'all,iheap,dmbtree'");
$primary->backup('b1');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'b1', has_streaming => 1);
$standby->start;

IheapTest::setup_tables($primary);
for my $round (1 .. 6)
{
	iheap_workload($primary, $round);
}

# maintenance: prune, vacuum (visibility map; the indexes by parallel
# workers, §21 D44), discard, rewrite, truncate
$primary->safe_psql('postgres', q{
SET min_parallel_index_scan_size = 0;
VACUUM (PARALLEL 3) ti;
UPDATE ti SET v = v + 1;
UPDATE th SET v = v + 1;
SELECT iheap_discard('ti');
CREATE TABLE tf (id int PRIMARY KEY, v int, s text) USING iheap;
INSERT INTO tf SELECT g, g, repeat('f', 50) FROM generate_series(1, 2000) g;
UPDATE tf SET v = v + 1 WHERE id % 2 = 0;
DELETE FROM tf WHERE id % 3 = 0;
VACUUM FULL tf;
CLUSTER tf USING tf_pkey;
CREATE TABLE tt (id int, v int) USING iheap;
INSERT INTO tt SELECT g, g FROM generate_series(1, 1000) g;
BEGIN; TRUNCATE tt; INSERT INTO tt VALUES (1, 1); COMMIT;
COPY tf (id, v, s) FROM PROGRAM 'seq 3001 4000 | sed "s/$/\t1\tc/"';
});
iheap_workload($primary, 7);

# dmbtree: a unique index whose keys change in place (marks set and
# cleared, entries removed by VACUUM and when leaf pages fill)
$primary->safe_psql('postgres', q{
CREATE TABLE tu (id int, code int, pad text) USING iheap;
CREATE UNIQUE INDEX tu_code ON tu USING dmbtree (code);
INSERT INTO tu SELECT g, g, repeat('p', 20) FROM generate_series(1, 3000) g;
UPDATE tu SET code = code + 10000 WHERE id % 2 = 0;
BEGIN; UPDATE tu SET code = -code WHERE id % 3 = 0; ROLLBACK;
UPDATE tu SET code = code - 10000 WHERE id % 4 = 0;
VACUUM tu;
UPDATE tu SET code = code + 20000 WHERE id % 5 = 0;
});
$primary->safe_psql('postgres', 'VACUUM');

# moves (a btree index's column changes) and deletions while a prepared
# transaction holds the horizon: the old rows' bodies stay on their pages,
# and the pruning that frees them later carries a conflict horizon (§21
# D40).  Rows with compact headers (§21 D43), from a rewrite: locked,
# updated in place, moved, deleted, and rolled back.
$primary->safe_psql('postgres', q{
CREATE TABLE tc (id int PRIMARY KEY, k int, v int, s text) USING iheap;
CREATE INDEX tc_k ON tc USING btree (k);
INSERT INTO tc SELECT g, g, g, 'c' || g FROM generate_series(1, 3000) g;
SET iheap.compact_headers = always;
VACUUM FULL tc;
BEGIN;
INSERT INTO tc VALUES (0, 0, 0, 'held');
PREPARE TRANSACTION 'held';
SELECT count(*) FROM (SELECT id FROM tc WHERE id % 7 = 0 FOR KEY SHARE) l;
UPDATE tc SET v = v + 1 WHERE id % 5 = 0;
UPDATE tc SET k = k + 10000 WHERE id % 11 = 0;
DELETE FROM tc WHERE id % 13 = 0;
BEGIN;
UPDATE tc SET v = -v WHERE id % 3 = 0;
DELETE FROM tc WHERE id % 4 = 0;
UPDATE tc SET k = -k WHERE id % 6 = 0;
SELECT count(*) FROM (SELECT id FROM tc WHERE id % 9 = 0 FOR UPDATE) l;
ROLLBACK;
VACUUM tc;
COMMIT PREPARED 'held';
UPDATE tc SET k = k + 1 WHERE id % 17 = 0;
VACUUM tc;
VACUUM FULL tc;
UPDATE tc SET v = v + 1 WHERE id % 19 = 0;
DELETE FROM tc WHERE id % 23 = 0;
});

# rows moving off full pages while a prepared transaction holds the horizon
# (§21 D45): once a page's bodies wait for a committed mover, the next rows
# to leave it take their bodies to undo and leave stubs at once, whose room
# the page gets back by compaction when the moves commit; moves to other
# pages write no LOCK record.  Some of them roll back.
$primary->safe_psql('postgres', q{
CREATE TABLE tv (id int PRIMARY KEY, k int, s text) USING iheap;
CREATE INDEX tv_k ON tv USING btree (k);
INSERT INTO tv SELECT g, g, repeat('v', 100) FROM generate_series(1, 2000) g;
BEGIN;
INSERT INTO tv VALUES (0, 0, 'held');
PREPARE TRANSACTION 'held_tv';
});
for my $i (1 .. 20)
{
	$primary->safe_psql('postgres',
		"UPDATE tv SET k = k + 10000 WHERE id % 20 = $i % 20");
}
$primary->safe_psql('postgres', q{
BEGIN;
UPDATE tv SET k = -k WHERE id % 7 = 0;
DELETE FROM tv WHERE id % 11 = 0;
ROLLBACK;
VACUUM tv;
COMMIT PREPARED 'held_tv';
VACUUM tv;
UPDATE tv SET k = k + 1 WHERE id % 3 = 0;
});

$primary->wait_for_catchup($standby);

# the structure checks, on both sides (the standby replays every page)
for my $t (qw(ti tf tt tu tc tv))
{
	is(iheap_verify($primary, $t), '', "structure checks of $t on the primary");
	is(iheap_verify($standby, $t), '', "structure checks of $t on the standby");
}

is(iheap_snapshot($standby, 'ti'), iheap_snapshot($primary, 'ti'),
	'standby has the same data');
is(iheap_snapshot($standby, 'ti'), iheap_snapshot($primary, 'th'),
	'and it matches heap');
is($standby->safe_psql('postgres', 'SELECT count(*), sum(v) FROM tf'),
	$primary->safe_psql('postgres', 'SELECT count(*), sum(v) FROM tf'),
	'rewritten table replicated');
is($standby->safe_psql('postgres', 'SELECT count(*), sum(k), sum(v) FROM tc'),
	$primary->safe_psql('postgres', 'SELECT count(*), sum(k), sum(v) FROM tc'),
	'table with compact headers replicated');
is($standby->safe_psql('postgres', q{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*), sum(k) FROM tv WHERE k > -1000000}),
	$primary->safe_psql('postgres', 'SELECT count(*), sum(k) FROM tv'),
	'rows moved off full pages under a held horizon replicated');
is($standby->safe_psql('postgres', q{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) FROM ti WHERE k > -1000000}),
	$primary->safe_psql('postgres', 'SELECT count(*) FROM ti'),
	'index scans on the standby');
is($standby->safe_psql('postgres', q{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*), sum(code) FROM tu WHERE code > -100000}),
	$primary->safe_psql('postgres', 'SELECT count(*), sum(code) FROM tu'),
	'dmbtree index scans on the standby');
is($standby->safe_psql('postgres', 'SELECT * FROM dmbtree_index_stats(\'tu_code\')'),
	$primary->safe_psql('postgres', 'SELECT * FROM dmbtree_index_stats(\'tu_code\')'),
	'dmbtree index replicated with its marks');

# a long query on the standby while the primary discards: either it
# finishes or it is canceled by a recovery conflict, never wrong results
my $bg = $standby->background_psql('postgres', on_error_stop => 0);
$bg->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
my $old = $bg->query_safe('SELECT count(*), sum(v) FROM ti');
$primary->safe_psql('postgres', 'UPDATE ti SET v = v + 1; UPDATE th SET v = v + 1; VACUUM ti');
$primary->wait_for_catchup($standby);
# the session may have been canceled or terminated by a recovery conflict
my $out = eval { $bg->query('SELECT count(*), sum(v) FROM ti') };
my $err = $@ . ($bg->{stderr} // '');
ok((defined $out && $out eq $old) || $err =~ /conflict|canceling|terminat|ended prematurely/,
	'old snapshot on the standby sees old data, or is canceled');
eval { $bg->quit };

# promotion: the standby becomes a writable primary with iheap tables
$standby->promote;
$standby->safe_psql('postgres', q{
UPDATE ti SET v = v + 1;
INSERT INTO ti VALUES (999999, 1, 1, 'after promote', NULL);
DELETE FROM ti WHERE id = 999999;
VACUUM ti;
});
is($standby->safe_psql('postgres', 'SELECT count(*) FROM ti WHERE id = 999999'),
	'0', 'writes after promotion');
is(iheap_verify($standby, 'ti'), '', 'structure checks after promotion');

# no consistency check failure ("inconsistent page found") in the log
my $log = slurp_file($standby->logfile);
unlike($log, qr/inconsistent page found/, 'WAL consistency checks passed');

$primary->stop;
$standby->stop;
done_testing();
