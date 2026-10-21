# Crashes inside rollback and discard, forced with injection points: while the
# aborting backend's own rollback waits after its abort is visible, while
# discard waits to apply the aborted records it collected, and while it waits
# to move its pointer past them.  A checkpoint writes the leftovers out first.
# After each crash the table matches its heap twin, the structure checks pass,
# writers and VACUUM clean up after it.  Needs a build with injection points.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IheapTest;

my $node = iheap_node('crash_races');
plan skip_all => 'injection points not supported by this build'
  unless $node->check_extension('injection_points');
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
IheapTest::setup_tables($node);
iheap_workload($node, 1);

sub same
{
	my ($what) = @_;
	is(iheap_snapshot($node, 'ti'), iheap_snapshot($node, 'th'),
		"$what: ti matches heap");
	is(iheap_verify($node, 'ti'), '', "$what: structure checks");
}

# Run $sql in a session that waits at injection point $point, write
# everything out, and crash.
sub crash_at
{
	my ($point, $setup, $sql, $what) = @_;
	my $s = $node->background_psql('postgres');
	$s->query_safe('SELECT injection_points_set_local()');
	$s->query_safe("SELECT injection_points_attach('$point', 'wait')");
	$s->query_safe($_) for @$setup;
	$s->query_until(qr/started/, "\\echo started\n$sql\n");
	$node->wait_for_event('client backend', $point);
	$node->safe_psql('postgres', 'CHECKPOINT');
	$node->stop('immediate');
	eval { $s->quit };
	$node->start;
	same($what);
	is($node->safe_psql('postgres',
		"SELECT count(*) FROM ti WHERE v > 1000000 OR v < 0 OR s = 'x'"),
		'0', "$what: the aborted changes are invisible");
}

# 1. The aborting backend's rollback waits, nothing of it applied
crash_at('iheap-eager-rollback', [
		'BEGIN',
		"UPDATE ti SET v = v + 1000000, s = 'x' WHERE id % 2 = 0",
		'DELETE FROM ti WHERE id % 3 = 0',
		"INSERT INTO ti SELECT g, g, g, 'x', NULL FROM generate_series(100001, 100100) g"
	],
	'ROLLBACK;', 'crash while the abort waits to roll back');
iheap_workload($node, 2);
same('writers after that crash');

# 2. Discard waits to apply the aborted records it collected
$node->safe_psql('postgres', q{
SET iheap.foreground_undo_limit = 0;
BEGIN;
UPDATE ti SET v = -v WHERE id % 5 = 0;
DELETE FROM ti WHERE id % 7 = 0;
ROLLBACK;
});
crash_at('iheap-discard-apply', [], "SELECT iheap_discard('ti');",
	'crash while discard waits to apply aborted records');

# 3. Discard applied them and waits to move its pointer past them
$node->safe_psql('postgres', q{
SET iheap.foreground_undo_limit = 0;
BEGIN;
UPDATE ti SET v = -v, s = 'x' WHERE id % 5 = 1;
ROLLBACK;
});
crash_at('iheap-discard-advance', [], "SELECT iheap_discard('ti');",
	'crash while discard waits to move its pointer');

iheap_workload($node, 3);
same('writers after the crashes');
$node->safe_psql('postgres', 'VACUUM ti');
is($node->safe_psql('postgres', "SELECT live_undo_pages FROM iheap_metapage_info('ti')"),
	'0', 'VACUUM discarded the undo');
same('after VACUUM');

$node->stop;
done_testing();
