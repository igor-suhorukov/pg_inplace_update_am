# Crash recovery with full_page_writes off.  Few shared buffers write pages
# out at any point of the workload, so redo meets pages that are newer than
# the record it replays (BLK_DONE), undo pages among them, some of them
# reused since; redo must still give the tuple headers it rebuilds the right
# undo pages.  Bodies of moved and deleted rows stay on their pages, and rows
# have compact headers.  The tables are compared with heap twins and checked.
#
# No wal_consistency_checking here: where an undo page has been reused since
# the record, redo gives the header undo pointer 0, which means what the
# primary's pointer to discarded undo means (frozen) in other bytes; and a
# record of a relation dropped later can extend its file past a block it
# did not find, which the check then reads as a zero page (heap's records
# too).  Without full-page writes, pages newer than the record only occur
# in crash recovery, before any query runs.
#
# wal_level replica: with minimal, the commit of a rewrite WAL-logs the new
# relation 32 pinned buffers at a time, more than this node has.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IheapTest;

my $node = iheap_node('fpw', allows_streaming => 1, extra_conf => q{
full_page_writes = off
shared_buffers = 128kB
checkpoint_timeout = 1h
max_wal_size = 1GB
});
IheapTest::setup_tables($node);

# tm and its heap twin tmh: a btree index, so that updates of k move rows
$node->safe_psql('postgres', q{
CREATE TABLE tm (id int PRIMARY KEY, k int, v int, s text) USING iheap;
CREATE INDEX tm_k ON tm USING btree (k);
CREATE TABLE tmh (id int PRIMARY KEY, k int, v int, s text) USING heap;
CREATE INDEX tmh_k ON tmh USING btree (k);
INSERT INTO tm SELECT g, g, g, repeat('m', g % 40) FROM generate_series(1, 3000) g;
INSERT INTO tmh SELECT g, g, g, repeat('m', g % 40) FROM generate_series(1, 3000) g;
SET iheap.compact_headers = always;
VACUUM FULL tm;
});

# the same statements on tm and tmh
sub moves
{
	my ($round) = @_;
	my $sql = '';
	for my $t ('tm', 'tmh')
	{
		$sql .= qq{
UPDATE $t SET k = k + 10000 WHERE id % 7 = $round % 7;
UPDATE $t SET v = v + 1 WHERE id % 5 = $round % 5;
DELETE FROM $t WHERE id % 31 = $round % 31 AND id > 100;
INSERT INTO $t SELECT g, g, g, 'i' || g
  FROM generate_series(5000 + $round * 40 + 1, 5000 + $round * 40 + 40) g;
BEGIN; UPDATE $t SET k = -k WHERE id % 3 = 0; DELETE FROM $t WHERE id % 4 = 0; ROLLBACK;
};
	}
	$node->safe_psql('postgres', $sql);
}

sub same
{
	my ($what) = @_;
	is(iheap_snapshot($node, 'ti'), iheap_snapshot($node, 'th'),
		"$what: ti matches heap");
	is($node->safe_psql('postgres',
		'SELECT count(*), sum(k), sum(v), md5(string_agg(id || \':\' || k || \':\' || v || \':\' || s, \',\' ORDER BY id)) FROM tm'),
	   $node->safe_psql('postgres',
		'SELECT count(*), sum(k), sum(v), md5(string_agg(id || \':\' || k || \':\' || v || \':\' || s, \',\' ORDER BY id)) FROM tmh'),
		"$what: tm matches heap");
	is($node->safe_psql('postgres', q{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) FROM tm WHERE k > -1000000}),
		$node->safe_psql('postgres', 'SELECT count(*) FROM tm'),
		"$what: index scan finds every row of tm");
	is(iheap_verify($node, 'ti'), '', "$what: structure checks of ti");
	is(iheap_verify($node, 'tm'), '', "$what: structure checks of tm");
}

# 1. Undo pages freed by a discard and reused before the crash: redo of the
#    records that wrote them before finds a page newer than the record, with
#    other records on it.
$node->safe_psql('postgres', 'CHECKPOINT');
for my $round (1 .. 3)
{
	iheap_workload($node, $round);
	moves($round);
}
$node->safe_psql('postgres', 'VACUUM ti; VACUUM tm');
for my $round (4 .. 6)
{
	iheap_workload($node, $round);
	moves($round);
}
$node->stop('immediate');
$node->start;
same('after the first crash');

# 2. A prepared transaction holds the horizon: moved and deleted rows keep
#    their bodies on their pages through VACUUM, and the pruning that frees
#    them after the commit comes before the crash.  An in-flight transaction
#    is rolled back after recovery.
$node->safe_psql('postgres', q{
CHECKPOINT;
BEGIN;
INSERT INTO tm VALUES (0, 0, 0, 'held');
INSERT INTO tmh VALUES (0, 0, 0, 'held');
PREPARE TRANSACTION 'held';
});
for my $round (7 .. 8)
{
	iheap_workload($node, $round);
	moves($round);
}
$node->safe_psql('postgres', q{
VACUUM tm;
COMMIT PREPARED 'held';
VACUUM tm;
SET iheap.compact_headers = always;
VACUUM FULL tm;
});
moves(9);
my $bg = $node->background_psql('postgres');
$bg->query_safe('BEGIN');
$bg->query_safe('UPDATE tm SET k = k + 50000, v = -v WHERE id % 2 = 0');
$bg->query_safe('DELETE FROM tm WHERE id % 3 = 0');
$bg->query_safe("UPDATE ti SET v = v + 1000000, s = repeat('crash', 30)");
$node->stop('immediate');
$bg->quit;
$node->start;
same('after the second crash');

# writers and VACUUM clean up after the crashed transaction
iheap_workload($node, 10);
moves(10);
$node->safe_psql('postgres', 'VACUUM ti; VACUUM tm');
same('after VACUUM');

# 3. A checkpoint in the middle of a transaction: the undo page it writes
#    into on either side of the checkpoint is written out later (redo of a
#    page allocated after the checkpoint initializes it, so only such a page
#    can be newer than a record).  First with a prepared transaction holding
#    the horizon, so that the page still has the records; then without, the
#    undo discarded and its page reused.
$node->safe_psql('postgres', 'CREATE TABLE hold (x int)');
my $round = 11;
for my $reuse (0, 1)
{
	$node->safe_psql('postgres',
		"BEGIN; INSERT INTO hold VALUES (1); PREPARE TRANSACTION 'pin'")
	  unless $reuse;
	my $w = $node->background_psql('postgres');
	$w->query_safe('BEGIN');
	$w->query_safe('UPDATE tm SET v = v + 1 WHERE id % 10 = 1');
	$w->query_safe('UPDATE tmh SET v = v + 1 WHERE id % 10 = 1');
	$node->safe_psql('postgres', 'CHECKPOINT');
	$w->query_safe('UPDATE tm SET v = v + 1, s = s || \'+\' WHERE id % 10 = 2');
	$w->query_safe('UPDATE tmh SET v = v + 1, s = s || \'+\' WHERE id % 10 = 2');
	$w->query_safe('COMMIT');
	$w->quit;
	moves($round++) for 1 .. 3;
	if ($reuse)
	{
		$node->safe_psql('postgres', 'VACUUM tm');
		moves($round++) for 1 .. 6;
	}
	$node->stop('immediate');
	$node->start;
	same($reuse ? 'undo page reused before the crash' : 'undo page written out before the crash');
	$node->safe_psql('postgres', "COMMIT PREPARED 'pin'") unless $reuse;
}

$node->stop;
done_testing();
