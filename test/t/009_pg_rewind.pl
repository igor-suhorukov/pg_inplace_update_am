# pg_rewind.  After a failover the old primary has written what the new
# primary never saw: in-place updates, moves and deletions, undo in new
# extents, a discard, a VACUUM that truncated a table, a rewrite, a table
# created and one dropped, aborted changes left to lazy rollback and a
# transaction cut off by an immediate stop.  pg_rewind copies back the blocks
# the old primary's WAL names since the timelines forked: first from the
# running new primary (after the crash recovery it runs itself), then, the
# roles swapped, from a stopped server's data directory.  The rewound node
# follows the other as a standby with the same data, which the structure
# checks find sound.  No data checksums here: wal_log_hints.
use strict;
use warnings FATAL => 'all';
use File::Copy;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IheapTest;

my $conf = q{
wal_log_hints = on
wal_keep_size = 1GB
};

my $a = iheap_node('a', allows_streaming => 1, no_data_checksums => 1,
	extra_conf => $conf);
IheapTest::setup_tables($a);
setup_move_tables($a);
$a->safe_psql('postgres', q{
CREATE TABLE tdrop (id int PRIMARY KEY, v int) USING iheap;
INSERT INTO tdrop SELECT g, g FROM generate_series(1, 1000) g;
CREATE TABLE ttail (id int PRIMARY KEY, s text) USING iheap;
INSERT INTO ttail SELECT g, repeat('t', 200) FROM generate_series(1, 5000) g;
});
for my $round (1 .. 2)
{
	iheap_workload($a, $round);
	iheap_moves($a, $round);
}
$a->backup('b');
my $b = PostgreSQL::Test::Cluster->new('b');
$b->init_from_backup($a, 'b', has_streaming => 1);
$b->start;
iheap_workload($a, 3);
iheap_moves($a, 3);
$a->wait_for_catchup($b);

sub wait_replayed
{
	my ($standby, $primary) = @_;
	my $lsn = $primary->lsn('flush');
	$standby->poll_query_until('postgres',
		"SELECT pg_last_wal_replay_lsn() >= '$lsn'::pg_lsn")
	  or die 'the standby did not catch up';
}

# pg_rewind copies the source's configuration files: keep the target's
# (but for what --write-recovery-conf writes into postgresql.auto.conf)
sub rewind
{
	my ($target, @args) = @_;
	my $dir = PostgreSQL::Test::Utils::tempdir();
	my @keep = ('postgresql.conf');
	push @keep, 'postgresql.auto.conf'
	  unless grep { $_ eq '--write-recovery-conf' } @args;
	for my $f (@keep)
	{
		copy($target->data_dir . "/$f", "$dir/$f") or die "copy $f: $!";
	}
	command_ok(
		[
			'pg_rewind', '--debug', '--no-sync',
			'--target-pgdata' => $target->data_dir,
			@args
		],
		'pg_rewind ' . join(' ', grep { /^--/ } @args));
	for my $f (@keep)
	{
		move("$dir/$f", $target->data_dir . "/$f") or die "move $f: $!";
	}
}

sub check
{
	my ($standby, $primary, $what) = @_;
	for my $t (qw(ti th tm tmh tdrop ttail tonly))
	{
		is(iheap_digest($standby, $t), iheap_digest($primary, $t),
			"$what: $t as on the primary");
	}
	is($standby->safe_psql('postgres', "SELECT to_regclass('tforked') IS NULL"),
		't', "$what: the table made on the other timeline is gone");
	is(iheap_snapshot($standby, 'ti'), iheap_snapshot($standby, 'th'),
		"$what: ti matches heap");
	is(iheap_digest($standby, 'tm'), iheap_digest($standby, 'tmh'),
		"$what: tm matches heap");
	is(iheap_digest($standby, 'ti', index => 1, where => 'k > -2147483648'),
		iheap_digest($standby, 'ti'), "$what: ti through its dmbtree index");
	is(iheap_digest($standby, 'tm', index => 1, where => 'k > -2147483648'),
		iheap_digest($standby, 'tm'), "$what: tm through its btree index");
	is(join('', map { iheap_verify($standby, $_) } qw(ti tm tdrop ttail tonly)),
		'', "$what: structure checks");
}

# what the old primary writes after the fork, lost by the rewind
sub forked_work
{
	my ($node, $round) = @_;
	iheap_workload($node, $round);
	iheap_moves($node, $round);
	$node->safe_psql('postgres', q{
DELETE FROM ttail WHERE id > 1000;
VACUUM ttail;
DROP TABLE tdrop;
CREATE TABLE tforked (id int PRIMARY KEY, v int) USING iheap;
INSERT INTO tforked SELECT g, g FROM generate_series(1, 1000) g;
SET iheap.compact_headers = auto;
VACUUM FULL tm;
UPDATE ti SET s = repeat('f', 300) WHERE id % 2 = 0;
VACUUM ti;
SELECT iheap_discard('ti');
SET iheap.foreground_undo_limit = 0;
BEGIN; UPDATE ti SET v = -v; DELETE FROM tm WHERE id % 2 = 1; ROLLBACK;
});
}

# what the new primary writes after the fork, kept
sub kept_work
{
	my ($node, $round) = @_;
	iheap_workload($node, $round);
	iheap_moves($node, $round);
	$node->safe_psql('postgres', q{
CREATE TABLE IF NOT EXISTS tonly (id int PRIMARY KEY, v int) USING iheap;
INSERT INTO tonly SELECT max(id) + g, g FROM generate_series(1, 500) g,
  (SELECT coalesce(max(id), 0) AS id FROM tonly) m GROUP BY g;
UPDATE ttail SET s = 'kept' WHERE id % 10 = 0;
VACUUM ti;
});
	iheap_workload($node, $round + 1);
}

# 1. b is promoted; a goes on as a primary of the old timeline and stops
#    with a transaction in flight.  pg_rewind takes b's blocks while b runs.
$b->promote;
forked_work($a, 4);
my $cut = $a->background_psql('postgres');
$cut->query_safe('BEGIN');
$cut->query_safe('UPDATE ti SET v = v + 5000000 WHERE id % 3 = 0');
$cut->query_safe(
	"INSERT INTO tm SELECT g, g, g, 'cut' FROM generate_series(300001, 301000) g");
kept_work($b, 5);
$a->stop('immediate');
$cut->quit;
rewind($a,
	'--source-server' => $b->connstr('postgres') . ' application_name=a',
	'--write-recovery-conf');
$a->start;
kept_work($b, 7);
wait_replayed($a, $b);
check($a, $b, 'rewound from a running server');

# 2. a is promoted too; both write; both stop.  b is rewound from a's data
#    directory and follows a.
$a->promote;
$a->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die 'a was not promoted';
kept_work($a, 9);
forked_work($b, 11);
$a->stop;
$b->stop;
rewind($b, '--source-pgdata' => $a->data_dir);
$b->set_standby_mode;
$a->start;
$b->start;
kept_work($a, 12);
wait_replayed($b, $a);
check($b, $a, 'rewound from a data directory');

$a->stop;
$b->stop;
done_testing();
