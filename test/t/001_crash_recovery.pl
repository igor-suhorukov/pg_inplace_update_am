# Crash recovery: committed changes survive, in-flight ones disappear, and
# the table stays usable (lazy rollback of the crashed transactions).
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IheapTest;

my $node = iheap_node('crash');
IheapTest::setup_tables($node);

# some history, a checkpoint in the middle
for my $round (1 .. 3)
{
	iheap_workload($node, $round);
}
$node->safe_psql('postgres', 'CHECKPOINT');
for my $round (4 .. 6)
{
	iheap_workload($node, $round);
}

# a prepared transaction, which must survive
$node->safe_psql('postgres', q{
BEGIN;
UPDATE ti SET v = v + 7 WHERE id = 1;
UPDATE th SET v = v + 7 WHERE id = 1;
PREPARE TRANSACTION 'p1';
});

# in-flight work at the time of the crash, large enough to need undo pages
# (row 1 belongs to the prepared transaction)
my $bg = $node->background_psql('postgres');
$bg->query_safe('BEGIN');
$bg->query_safe("UPDATE ti SET v = v + 1000000, s = repeat('crash', 30) WHERE id <> 1");
$bg->query_safe("UPDATE ti SET k = -k - 5000000 WHERE id % 3 = 0 AND id <> 1");
$bg->query_safe('DELETE FROM ti WHERE id % 2 = 0');
$bg->query_safe("INSERT INTO ti SELECT g, g, g, 'lost', NULL FROM generate_series(100001, 101000) g");

$node->stop('immediate');
$bg->quit;
$node->start;

is(iheap_snapshot($node, 'ti'), iheap_snapshot($node, 'th', where => 'true'),
	'committed state after crash recovery matches heap');
is($node->safe_psql('postgres', "SELECT count(*) FROM ti WHERE s = 'lost' OR v > 1000000"),
	'0', 'changes of the crashed transaction are invisible');
is($node->safe_psql('postgres', q{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) FROM ti WHERE k < -1000000}),
	'0', 'key changes of the crashed transaction are not found by the index');

# the prepared transaction is still there and can commit
$node->safe_psql('postgres', "COMMIT PREPARED 'p1'");
is(iheap_snapshot($node, 'ti'), iheap_snapshot($node, 'th'),
	'prepared transaction committed after recovery');

# writers roll back the crashed transaction's leftovers as they go
iheap_workload($node, 7);
is(iheap_snapshot($node, 'ti'), iheap_snapshot($node, 'th'),
	'updates after recovery');

# indexes agree with the table
is($node->safe_psql('postgres', q{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) FROM ti WHERE k > -1000000}),
	$node->safe_psql('postgres', 'SELECT count(*) FROM ti'),
	'index scan finds every row');

# VACUUM discards everything and freezes every row, and removes the
# crashed transaction's index entries
$node->safe_psql('postgres', 'VACUUM ti');
is($node->safe_psql('postgres',
	'SELECT tids = (SELECT count(*) FROM ti), marked FROM dmbtree_index_stats(\'ti_k\')'),
	't|0', 'dmbtree entries cleaned up');
is($node->safe_psql('postgres', 'SELECT live_undo_pages FROM iheap_metapage_info(\'ti\')'),
	'0', 'undo discarded after VACUUM');
is($node->safe_psql('postgres',
	'SELECT live_items = frozen_items FROM iheap_relation_stats(\'ti\')'),
	't', 'all rows frozen');

# a second crash right after a VACUUM and more work
iheap_workload($node, 8);
$node->stop('immediate');
$node->start;
is(iheap_snapshot($node, 'ti'), iheap_snapshot($node, 'th'),
	'second crash recovery');

# unlogged tables come back empty but usable
$node->safe_psql('postgres', q{
CREATE UNLOGGED TABLE tu (id int PRIMARY KEY, v int) USING iheap;
INSERT INTO tu SELECT g, g FROM generate_series(1, 1000) g;
UPDATE tu SET v = v + 1;
CHECKPOINT;
});
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', 'SELECT count(*) FROM tu'), '0',
	'unlogged table is empty after crash');
$node->safe_psql('postgres', q{
INSERT INTO tu SELECT g, g FROM generate_series(1, 100) g;
UPDATE tu SET v = v * 2;
});
is($node->safe_psql('postgres', 'SELECT sum(v) FROM tu'), '10100',
	'unlogged table usable after crash');

$node->stop;
done_testing();
