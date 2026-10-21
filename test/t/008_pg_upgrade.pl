# pg_upgrade: clusters with iheap tables upgraded to new clusters of the same
# version, in copy, link and swap modes.  The old cluster holds what iheap
# leaves on its pages: undo chains of in-place updates, moved and deleted rows
# whose bodies wait on the page or went to undo, compact headers, aborted
# changes left to lazy rollback and a crashed transaction's leftovers (the new
# cluster takes over the old one's clog); dmbtree indexes (expressions, a
# predicate with INCLUDE, unique, constraint indexes made by
# iheap.unique_index_am), TOAST, partitions, a materialized view, an unlogged
# table and a second database.  After the upgrade the tables hold the same
# rows, the structure checks pass, pg_dumpall writes what it wrote before,
# writers roll back the leftovers and VACUUM discards the undo.  A new cluster
# with another iheap.unique_index_am gives the constraint indexes the other
# access method: the index file's magic number makes that an error, never a
# misread, until REINDEX.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IheapTest;

# pg_upgrade writes its scripts and logs into the current directory
chdir ${PostgreSQL::Test::Utils::tmp_check};

my $conf = "iheap.unique_index_am = dmbtree\n";
my @tables = qw(ti th tm tmh tu tp mv tul);

sub populate
{
	my ($old) = @_;
	IheapTest::setup_tables($old);
	setup_move_tables($old);
	for my $round (1 .. 3)
	{
		iheap_workload($old, $round);
		iheap_moves($old, $round);
	}


	$old->safe_psql('postgres', q{
-- a unique dmbtree index whose keys change in place
CREATE TABLE tu (id int, code int, pad text) USING iheap;
CREATE UNIQUE INDEX tu_code ON tu USING dmbtree (code);
INSERT INTO tu SELECT g, g, repeat('p', 20) FROM generate_series(1, 3000) g;
UPDATE tu SET code = code + 10000 WHERE id % 2 = 0;
-- partitions, a materialized view, an unlogged table
CREATE TABLE tp (id int, k int, s text) PARTITION BY RANGE (id) USING iheap;
CREATE TABLE tp1 PARTITION OF tp FOR VALUES FROM (0) TO (1000);
CREATE TABLE tp2 PARTITION OF tp FOR VALUES FROM (1000) TO (MAXVALUE);
CREATE INDEX tp_k ON tp (k);
INSERT INTO tp SELECT g, g, 'p' || g FROM generate_series(1, 2000) g;
UPDATE tp SET k = k + 1, s = s || 'u' WHERE id % 3 = 0;
CREATE MATERIALIZED VIEW mv USING iheap AS SELECT id, k, s FROM ti WHERE id % 10 = 0;
CREATE DATABASE db2;
});
	iheap_setup_db($old, 'db2');
	$old->safe_psql('db2', q{
CREATE TABLE t2 (id int PRIMARY KEY, v int, s text) USING iheap;
INSERT INTO t2 SELECT g, g, repeat('d', g % 100) FROM generate_series(1, 2000) g;
UPDATE t2 SET v = v * 2 WHERE id % 3 = 0;
DELETE FROM t2 WHERE id % 7 = 0;
});
	iheap_workload($old, 4);

	# moves and deletions under a held snapshot: the bodies of the rows that
	# leave stay on their pages or go to undo
	my $snap = $old->background_psql('postgres');
	$snap->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$snap->query_safe('SELECT count(*) FROM tm');
	iheap_moves($old, 4);
	iheap_moves($old, 5);
	$snap->query_safe('COMMIT');
	$snap->quit;

	# aborted changes left to lazy rollback
	$old->safe_psql('postgres', q{
SET iheap.foreground_undo_limit = 0;
BEGIN;
UPDATE ti SET v = v + 1000000, s = repeat('lost', 30) WHERE id % 4 = 0;
DELETE FROM ti WHERE id % 9 = 0;
INSERT INTO ti SELECT g, g, g, 'lost', NULL FROM generate_series(100001, 100500) g;
UPDATE tm SET k = -k WHERE id % 5 = 0;
UPDATE tu SET code = -code WHERE id % 3 = 0;
ROLLBACK;
});

	# a crashed transaction's changes, written out before the crash
	my $crash = $old->background_psql('postgres');
	$crash->query_safe('BEGIN');
	$crash->query_safe(
		"UPDATE ti SET v = v + 2000000, s = repeat('crash', 30) WHERE id % 4 = 1");
	$crash->query_safe('DELETE FROM tm WHERE id % 6 = 0');
	$crash->query_safe(
		"INSERT INTO tm SELECT g, g, g, 'crash' FROM generate_series(200001, 200500) g");
	$old->safe_psql('postgres', 'CHECKPOINT');
	$old->stop('immediate');
	$crash->quit;
	$old->start;

	# (after the crash, which empties unlogged tables)
	$old->safe_psql('postgres', q{
CREATE UNLOGGED TABLE tul (id int PRIMARY KEY, v int) USING iheap;
INSERT INTO tul SELECT g, g FROM generate_series(1, 500) g;
UPDATE tul SET v = v + 1 WHERE id % 2 = 0;
});
	ok($old->safe_psql('postgres',
		"SELECT live_undo_pages > 0 FROM iheap_metapage_info('ti')") eq 't',
		'the old cluster has undo');
}

sub contents
{
	my ($node) = @_;
	my %c = map { $_ => iheap_digest($node, $_) } @tables;
	$c{t2} = iheap_digest($node, 't2', db => 'db2');
	return \%c;
}

sub dumpall
{
	my ($node, $file) = @_;
	$node->command_ok(
		[
			'pg_dumpall', '--no-sync', '--no-statistics',
			'--restrict-key' => 'test',
			'--file' => $file
		],
		"pg_dumpall of " . $node->name);
}

sub upgrade
{
	my ($old, $mode, $newconf, $name) = @_;
	my $new = PostgreSQL::Test::Cluster->new($name);
	$new->init;
	iheap_conf($new, $newconf);
	my @cmd = (
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $old->data_dir,
		'--new-datadir' => $new->data_dir,
		'--old-bindir' => $old->config_data('--bindir'),
		'--new-bindir' => $new->config_data('--bindir'),
		'--socketdir' => $new->host,
		'--old-port' => $old->port,
		'--new-port' => $new->port);
	command_ok([ @cmd, '--check' ], "$name: pg_upgrade --check");
	command_ok([ @cmd, $mode ], "$name: pg_upgrade $mode");
	$new->start;
	return $new;
}

sub check_upgraded
{
	my ($new, $before, $dump_before, $what) = @_;

	is_deeply(contents($new), $before, "$what: the tables hold the same rows");
	is(iheap_snapshot($new, 'ti'), iheap_snapshot($new, 'th'),
		"$what: ti matches heap");
	is(iheap_digest($new, 'tm'), iheap_digest($new, 'tmh'),
		"$what: tm matches heap");
	is($new->safe_psql('postgres', q{
SELECT count(*) FROM ti WHERE v > 1000000 OR s LIKE 'lost%' OR s LIKE 'crash%'}),
		'0', "$what: aborted and crashed changes are invisible");
	is(iheap_digest($new, 'ti', index => 1, where => 'k > -2147483648'),
		iheap_digest($new, 'ti'), "$what: ti through its dmbtree index");
	is(iheap_digest($new, 'ti', index => 1, where => 'id > 0'),
		iheap_digest($new, 'ti'), "$what: ti through its primary key");
	is(iheap_digest($new, 'tm', index => 1, where => 'k > -2147483648'),
		iheap_digest($new, 'tm'), "$what: tm through its btree index");
	is(iheap_digest($new, 'tu', index => 1, where => 'code > -2147483648'),
		iheap_digest($new, 'tu'), "$what: tu through its unique index");
	is($new->safe_psql('postgres', q{
SELECT string_agg(c.relname || ':' || a.amname, ',' ORDER BY c.relname)
FROM pg_class c JOIN pg_am a ON a.oid = c.relam
WHERE c.relname IN ('ti_pkey', 'ti_k', 'tm_pkey', 'tm_k', 'th_pkey', 'tu_code')}),
		'th_pkey:btree,ti_k:dmbtree,ti_pkey:dmbtree,tm_k:btree,tm_pkey:dmbtree,tu_code:dmbtree',
		"$what: the indexes keep their access methods");
	is(join('', map { iheap_verify($new, $_) } qw(ti tm tu tp1 tp2 mv tul)),
		'', "$what: structure checks");
	is(iheap_verify($new, 't2', db => 'db2'), '',
		"$what: structure checks in the second database");

	my $dump_after = "${dump_before}_$what";
	dumpall($new, $dump_after);
	compare_files($dump_before, $dump_after,
		"$what: pg_dumpall writes what it wrote before the upgrade");

	# writers roll back the leftovers; VACUUM discards the undo and
	# freezes every row
	iheap_workload($new, 10);
	iheap_moves($new, 10);
	is(iheap_snapshot($new, 'ti'), iheap_snapshot($new, 'th'),
		"$what: ti matches heap after more changes");
	is(iheap_digest($new, 'tm'), iheap_digest($new, 'tmh'),
		"$what: tm matches heap after more changes");
	is($new->safe_psql('postgres', "SELECT rollback_lazy > 0 FROM iheap_stat('ti')"),
		't', "$what: writers rolled back what the old cluster left");
	$new->safe_psql('postgres', 'VACUUM');
	is($new->safe_psql('postgres', q{
SELECT sum(live_undo_pages) FROM (VALUES ('ti'), ('tm'), ('tu')) t(r),
  LATERAL iheap_metapage_info(r::regclass)}),
		'0', "$what: VACUUM discarded the undo");
	is($new->safe_psql('postgres',
		"SELECT live_items = frozen_items FROM iheap_relation_stats('ti')"),
		't', "$what: every row of ti frozen");
	is(join('', map { iheap_verify($new, $_) } qw(ti tm tu)),
		'', "$what: structure checks after VACUUM");
}

# 1. Copy mode, which leaves the old cluster as it was: upgraded once with
#    the old cluster's settings, and once with btree for constraint indexes.
my $old = iheap_node('old_copy', extra_conf => $conf);
populate($old);
my $before = contents($old);
my $dump_before = "${PostgreSQL::Test::Utils::tmp_check}/dump_copy.sql";
dumpall($old, $dump_before);
$old->stop;

my $new = upgrade($old, '--copy', $conf, 'new_copy');
check_upgraded($new, $before, $dump_before, 'copy');
$new->stop;

$new = upgrade($old, '--copy', '', 'new_btree');
is($new->safe_psql('postgres', q{
SELECT a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
WHERE c.relname = 'ti_pkey'}),
	'btree', 'other setting: the constraint index is made with it');
my ($ret, $out, $err) = $new->psql('postgres',
	'SET enable_seqscan = off; SELECT count(*) FROM ti WHERE id = 5');
like($err, qr/index "ti_pkey" is not a btree/,
	'other setting: the dmbtree file under a btree index is an error');
($ret, $out, $err) =
  $new->psql('postgres', "SELECT bt_index_check('ti_pkey')");
like($err, qr/index "ti_pkey" is not a btree/,
	'other setting: amcheck reports it');
$new->safe_psql('postgres', 'REINDEX INDEX ti_pkey');
is(iheap_digest($new, 'ti', index => 1, where => 'id > 0'), $before->{ti},
	'other setting: REINDEX repairs the index');
is(iheap_verify($new, 'ti'), '', 'other setting: structure checks after REINDEX');
$new->stop;

# 2. Link and swap modes, each from a cluster of its own
for my $mode ('link', 'swap')
{
	$old = iheap_node("old_$mode", extra_conf => $conf);
	populate($old);
	$before = contents($old);
	$dump_before = "${PostgreSQL::Test::Utils::tmp_check}/dump_$mode.sql";
	dumpall($old, $dump_before);
	$old->stop;
	$new = upgrade($old, "--$mode", $conf, "new_$mode");
	check_upgraded($new, $before, $dump_before, $mode);
	$new->stop;
}

done_testing();
