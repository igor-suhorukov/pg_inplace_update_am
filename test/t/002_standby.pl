# Streaming replication with wal_consistency_checking: every iheap record
# is replayed on a standby and the result compared with the primary's page
# image.  Queries on the standby see the same data.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IheapTest;

# 'iheap' is a custom resource manager: named explicitly ("all" is
# expanded before shared_preload_libraries are loaded)
my $primary = iheap_node('primary', allows_streaming => 1,
	extra_conf => "wal_consistency_checking = 'all,iheap'");
$primary->backup('b1');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'b1', has_streaming => 1);
$standby->start;

IheapTest::setup_tables($primary);
for my $round (1 .. 6)
{
	iheap_workload($primary, $round);
}

# maintenance: prune, vacuum (visibility map), discard, rewrite, truncate
$primary->safe_psql('postgres', q{
VACUUM ti;
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
$primary->safe_psql('postgres', 'VACUUM');

$primary->wait_for_catchup($standby);

is(iheap_snapshot($standby, 'ti'), iheap_snapshot($primary, 'ti'),
	'standby has the same data');
is(iheap_snapshot($standby, 'ti'), iheap_snapshot($primary, 'th'),
	'and it matches heap');
is($standby->safe_psql('postgres', 'SELECT count(*), sum(v) FROM tf'),
	$primary->safe_psql('postgres', 'SELECT count(*), sum(v) FROM tf'),
	'rewritten table replicated');
is($standby->safe_psql('postgres', q{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) FROM ti WHERE k > -1000000}),
	$primary->safe_psql('postgres', 'SELECT count(*) FROM ti'),
	'index scans on the standby');

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

# no consistency check failure ("inconsistent page found") in the log
my $log = slurp_file($standby->logfile);
unlike($log, qr/inconsistent page found/, 'WAL consistency checks passed');

$primary->stop;
$standby->stop;
done_testing();
