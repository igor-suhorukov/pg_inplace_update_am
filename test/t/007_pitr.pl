# Point-in-time recovery.  Base backups taken while transactions write, one of
# them incremental (pg_combinebackup), archived WAL, and recoveries to a
# restore point, an xid (inclusive and not), an LSN and the end of the
# archive.  At each target transactions are in flight, one of them prepared,
# aborted changes wait for lazy rollback, and undo has been discarded, pages
# pruned and a table rewritten before it.  The iheap tables then match their
# heap twins, which the same transactions changed, and the twins match what
# the primary had at the target; a recovery paused at a target shows the same
# to hot standby queries; after promotion the new timeline keeps working.  The
# primary's xids are of epoch 1, and its iheap and dmbtree records carry page
# images for wal_consistency_checking.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;
use IheapTest;

my $primary = iheap_node('primary', allows_streaming => 1, has_archiving => 1,
	epoch => 1, extra_conf => q{
wal_consistency_checking = 'iheap,dmbtree'
summarize_wal = on
max_connections = 30
});
IheapTest::setup_tables($primary);
setup_move_tables($primary);
# which targets a state includes
$primary->safe_psql('postgres', 'CREATE TABLE marks (name text PRIMARY KEY) USING heap');

sub work
{
	my ($node, $round) = @_;
	iheap_workload($node, $round);
	iheap_moves($node, $round);
}

# what a recovery must reproduce: the heap twins and the marks
sub state_of
{
	my ($node) = @_;
	return join(' / ', map { iheap_digest($node, $_) } qw(th tmh marks));
}

sub check_state
{
	my ($node, $expected, $what) = @_;
	is(state_of($node), $expected, "$what: the primary's state at the target");
	is(iheap_snapshot($node, 'ti'), iheap_snapshot($node, 'th'),
		"$what: ti matches heap");
	is(iheap_digest($node, 'tm'), iheap_digest($node, 'tmh'),
		"$what: tm matches heap");
	is(iheap_digest($node, 'ti', index => 1, where => 'k > -2147483648'),
		iheap_digest($node, 'th'), "$what: ti through its index");
	is(iheap_digest($node, 'tm', index => 1, where => 'k > -2147483648'),
		iheap_digest($node, 'tmh'), "$what: tm through its index");
	is(iheap_verify($node, 'ti') . iheap_verify($node, 'tm'), '',
		"$what: structure checks");
}

for my $round (1 .. 2)
{
	work($primary, $round);
}

# 1. The full backup, taken while pgbench changes both twins, and while a
#    transaction that commits after it has written.
my $script = PostgreSQL::Test::Utils::tempdir() . '/twins.sql';
open(my $fh, '>', $script) or die "could not write $script: $!";
print $fh q{
\set a random(1, 1000)
\set b random(1, 3000)
BEGIN;
UPDATE ti SET v = v + 1, s = repeat('b', 10 + :b % 50) WHERE id = :a;
UPDATE th SET v = v + 1, s = repeat('b', 10 + :b % 50) WHERE id = :a;
UPDATE tm SET k = k + 1, v = v - 1 WHERE id = :b;
UPDATE tmh SET k = k + 1, v = v - 1 WHERE id = :b;
COMMIT;
};
close($fh);
my $spanning = $primary->background_psql('postgres');
$spanning->query_safe('BEGIN');
# (rows pgbench leaves alone)
$spanning->query_safe("UPDATE ti SET s = 'spanning' WHERE id > 1000");
$spanning->query_safe("UPDATE th SET s = 'spanning' WHERE id > 1000");
my ($bench_out, $bench_err) = ('', '');
my $bench;
{
	# (the framework names every connection after the script)
	local $ENV{PGAPPNAME} = 'twins';
	$bench = IPC::Run::start(
		[
			'pgbench', '--no-vacuum', '--client' => 4, '--jobs' => 2,
			'--time' => 3, '--file' => $script,
			'--host' => $primary->host, '--port' => $primary->port, 'postgres'
		],
		'>' => \$bench_out,
		'2>' => \$bench_err);
}
$primary->poll_query_until('postgres',
	"SELECT count(*) = 4 FROM pg_stat_activity WHERE application_name = 'twins'")
  or die 'pgbench did not start';
$primary->backup('full');
$bench->finish;
like($bench_out, qr/number of failed transactions: 0 /,
	'pgbench ran through the backup');
$spanning->query_safe('COMMIT');

# 2. Restore point rp1: aborted changes left to lazy rollback (on the
#    iheap tables only: the twins are compared after the rollback), a
#    transaction in flight.
work($primary, 3);
$primary->safe_psql('postgres', q{
SET iheap.foreground_undo_limit = 0;
BEGIN;
UPDATE ti SET v = v + 1000000, s = repeat('lost', 30);
DELETE FROM ti WHERE id % 2 = 0;
UPDATE tm SET k = -k WHERE id % 3 = 0;
INSERT INTO tm SELECT g, g, g, 'lost' FROM generate_series(100001, 101000) g;
ROLLBACK;
});
my $inflight = $primary->background_psql('postgres');
$inflight->query_safe('BEGIN');
$inflight->query_safe('UPDATE ti SET v = v + 100 WHERE id % 3 = 0');
$inflight->query_safe('UPDATE th SET v = v + 100 WHERE id % 3 = 0');
$inflight->query_safe('UPDATE tm SET k = k + 50000 WHERE id % 4 = 0');
$inflight->query_safe('UPDATE tmh SET k = k + 50000 WHERE id % 4 = 0');
$primary->safe_psql('postgres', q{
INSERT INTO marks VALUES ('rp1');
SELECT pg_create_restore_point('rp1');
});
my $state_rp1 = state_of($primary);
$inflight->query_safe('COMMIT');

# 3. Target xid: undo discarded and pages pruned before it; a transaction
#    in flight across its commit.
$primary->safe_psql('postgres', 'VACUUM ti; VACUUM tm; SELECT iheap_discard(\'ti\')');
work($primary, 4);
$inflight->query_safe('BEGIN');
$inflight->query_safe('UPDATE ti SET v = v - 7 WHERE id % 5 = 0');
$inflight->query_safe('UPDATE th SET v = v - 7 WHERE id % 5 = 0');
my $target = $primary->background_psql('postgres');
$target->query_safe('BEGIN');
$target->query_safe('UPDATE ti SET k = k + 3 WHERE id % 5 = 1');
$target->query_safe('UPDATE th SET k = k + 3 WHERE id % 5 = 1');
$target->query_safe('UPDATE tm SET k = k + 3, s = \'t\' WHERE id % 6 = 0');
$target->query_safe('UPDATE tmh SET k = k + 3, s = \'t\' WHERE id % 6 = 0');
$target->query_safe("INSERT INTO marks VALUES ('xid')");
my $xid = $target->query_safe('SELECT pg_current_xact_id()');
my $state_before_xid = state_of($primary);
$target->query_safe('COMMIT');
my $state_xid = state_of($primary);
$inflight->query_safe('COMMIT');

# 4. The incremental backup, after a rewrite (bulk writer); target
#    LSN with a transaction in flight that later aborts.
$primary->safe_psql('postgres', q{
SET iheap.compact_headers = auto;
VACUUM FULL tm;
CLUSTER ti USING ti_pkey;
});
work($primary, 5);
$primary->backup('incr',
	backup_options =>
	  [ '--incremental' => $primary->backup_dir . '/full/backup_manifest' ]);
work($primary, 6);
$inflight->query_safe('BEGIN');
$inflight->query_safe('DELETE FROM ti WHERE id % 7 = 0');
$inflight->query_safe('DELETE FROM th WHERE id % 7 = 0');
$inflight->query_safe('UPDATE tm SET k = k - 1 WHERE id % 7 = 0');
$inflight->query_safe('UPDATE tmh SET k = k - 1 WHERE id % 7 = 0');
$primary->safe_psql('postgres', "INSERT INTO marks VALUES ('lsn')");
my $lsn = $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
my $state_lsn = state_of($primary);
$inflight->query_safe('ROLLBACK');

# 5. Restore point rp2 with a prepared transaction, committed after it.
work($primary, 7);
$primary->safe_psql('postgres', q{
BEGIN;
UPDATE ti SET v = v + 9 WHERE id % 8 = 0;
UPDATE th SET v = v + 9 WHERE id % 8 = 0;
UPDATE tm SET k = k + 9 WHERE id % 8 = 0;
UPDATE tmh SET k = k + 9 WHERE id % 8 = 0;
PREPARE TRANSACTION 'p1';
INSERT INTO marks VALUES ('rp2');
SELECT pg_create_restore_point('rp2');
});
my $state_rp2 = state_of($primary);
$primary->safe_psql('postgres', "COMMIT PREPARED 'p1'");
my $state_p1 = state_of($primary);

# 6. More work, the end of the archive.
work($primary, 8);
$primary->safe_psql('postgres', 'VACUUM ti; VACUUM tm');
work($primary, 9);
my $state_end = state_of($primary);
$spanning->quit;
$inflight->quit;
$target->quit;
my $walfile = $primary->safe_psql('postgres', 'SELECT pg_walfile_name(pg_switch_wal())');
$primary->poll_query_until('postgres',
	"SELECT '$walfile' <= last_archived_wal FROM pg_stat_archiver")
  or die 'WAL was not archived';
$primary->stop;

# A node recovered from a backup to a target.  It does not archive (the
# archive holds the primary's timeline only) and stays on that timeline.
my $nodes = 0;
sub recover
{
	my ($backup, $conf, %opts) = @_;
	my $node = PostgreSQL::Test::Cluster->new('pitr' . ++$nodes);
	$node->init_from_backup($primary, $backup,
		has_restoring => 1,
		standby => 0,
		$opts{combine} ? (combine_with_prior => $opts{combine}) : ());
	$node->append_conf('postgresql.conf', qq{
archive_mode = off
recovery_target_timeline = 'current'
recovery_target_action = '} . ($opts{pause} ? 'pause' : 'promote') . qq{'
$conf
});
	$node->start;
	if ($opts{pause})
	{
		$node->poll_query_until('postgres',
			"SELECT pg_get_wal_replay_pause_state() = 'paused'")
		  or die 'recovery did not pause';
	}
	else
	{
		$node->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
		  or die 'recovery did not end';
	}
	return $node;
}

# writers, VACUUM and discard on the new timeline
sub after_promotion
{
	my ($node, $what) = @_;
	work($node, 20);
	$node->safe_psql('postgres', 'VACUUM ti; VACUUM tm');
	work($node, 21);
	is(iheap_snapshot($node, 'ti'), iheap_snapshot($node, 'th'),
		"$what: ti matches heap after work on the new timeline");
	is(iheap_digest($node, 'tm'), iheap_digest($node, 'tmh'),
		"$what: tm matches heap after work on the new timeline");
	is(iheap_verify($node, 'ti') . iheap_verify($node, 'tm'), '',
		"$what: structure checks on the new timeline");
	unlike(slurp_file($node->logfile), qr/inconsistent page found/,
		"$what: WAL consistency checks passed");
	$node->teardown_node;
}

my $node = recover('full', "recovery_target_name = 'rp1'");
check_state($node, $state_rp1, 'restore point rp1');
after_promotion($node, 'rp1');

$node = recover('full', "recovery_target_xid = '$xid'");
check_state($node, $state_xid, 'xid, inclusive');
after_promotion($node, 'xid');

$node = recover('full',
	"recovery_target_xid = '$xid'\nrecovery_target_inclusive = off");
check_state($node, $state_before_xid, 'xid, not inclusive');
$node->teardown_node;

$node = recover('incr', "recovery_target_lsn = '$lsn'", combine => ['full']);
check_state($node, $state_lsn, 'LSN, from the incremental backup');
after_promotion($node, 'lsn');

# paused at rp2: hot standby queries see the state there, and the prepared
# transaction commits after the promotion
$node = recover('incr', "recovery_target_name = 'rp2'",
	combine => ['full'], pause => 1);
check_state($node, $state_rp2, 'restore point rp2, paused');
$node->safe_psql('postgres', 'SELECT pg_wal_replay_resume()');
$node->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die 'recovery did not end';
$node->safe_psql('postgres', "COMMIT PREPARED 'p1'");
check_state($node, $state_p1, 'rp2, prepared transaction committed');
after_promotion($node, 'rp2');

$node = recover('full', '');
check_state($node, $state_end, 'end of the archive');
after_promotion($node, 'end');

done_testing();
