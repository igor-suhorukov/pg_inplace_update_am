# Logical replication of iheap tables (DESIGN.md §9.5, §21 D38): changes of
# every kind made on a publisher -- in place, moving rows, through dmbtree
# keys, deletions, values stored out of line, COPY, upserts, transactions
# and subtransactions that roll back -- arrive at a subscriber whose tables
# are iheap too, where they agree with the publisher's tables and with heap
# twins.  A large transaction is streamed while in progress.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IheapTest;

my $pub = iheap_node('pub', allows_streaming => 'logical', extra_conf => q{
logical_decoding_work_mem = 64kB
});
my $sub = iheap_node('sub');

IheapTest::setup_tables($pub);
IheapTest::setup_tables($sub);
my $other = q{
CREATE TABLE tf (id int, v int, s text) USING iheap;
ALTER TABLE tf REPLICA IDENTITY FULL;
CREATE TABLE tc (id int PRIMARY KEY, n int, note text) USING iheap;
};
$pub->safe_psql('postgres', $other);
$sub->safe_psql('postgres', $other);
# the subscription copies the publisher's rows
$sub->safe_psql('postgres', 'TRUNCATE ti, th');

$pub->safe_psql('postgres', 'CREATE PUBLICATION p FOR TABLE ti, th, tf, tc');
my $connstr = $pub->connstr . ' dbname=postgres';
$sub->safe_psql('postgres',
	"CREATE SUBSCRIPTION s CONNECTION '$connstr' PUBLICATION p WITH (streaming = on)");
$sub->wait_for_subscription_sync($pub, 's');

is(iheap_snapshot($sub, 'ti'), iheap_snapshot($pub, 'ti'), 'initial copy');

for my $round (1 .. 4)
{
	iheap_workload($pub, $round);
}

# no primary key, the whole old row as identity
$pub->safe_psql('postgres', q{
INSERT INTO tf SELECT g, g, 'f' || g FROM generate_series(1, 200) g;
UPDATE tf SET v = v + 1 WHERE id % 2 = 0;
UPDATE tf SET s = s || repeat('y', 50) WHERE id % 7 = 0;
DELETE FROM tf WHERE id % 10 = 0;
});

# COPY, upserts, and a transaction large enough to be streamed
$pub->safe_psql('postgres', q{
COPY tc FROM PROGRAM 'seq 1 300 | sed -e "s/.*/&\t&\tc&/"';
INSERT INTO tc VALUES (1, -1, 'dup'), (1000, 1000, 'new')
  ON CONFLICT (id) DO UPDATE SET n = excluded.n, note = tc.note || '+';
INSERT INTO tc VALUES (2, -2, 'dup') ON CONFLICT DO NOTHING;
BEGIN;
UPDATE ti SET s = s || 'x';
UPDATE th SET s = s || 'x';
UPDATE tc SET note = note || repeat('z', 100);
SAVEPOINT a;
DELETE FROM tc WHERE id <= 100;
ROLLBACK TO a;
DELETE FROM tc WHERE id BETWEEN 101 AND 110;
COMMIT;
BEGIN;
DELETE FROM ti;
DELETE FROM tc;
ROLLBACK;
});

$pub->wait_for_catchup('s');

for my $t ('ti', 'th')
{
	is(iheap_snapshot($sub, $t), iheap_snapshot($pub, $t), "$t replicated");
}
is(iheap_snapshot($sub, 'ti'), iheap_snapshot($sub, 'th'),
	'the iheap subscriber table matches its heap twin');
for my $t ('tf', 'tc')
{
	is($sub->safe_psql('postgres', "SELECT md5(string_agg(${t}::text, ',' ORDER BY id)) FROM $t"),
		$pub->safe_psql('postgres', "SELECT md5(string_agg(${t}::text, ',' ORDER BY id)) FROM $t"),
		"$t replicated");
}
ok($pub->poll_query_until('postgres',
	"SELECT stream_txns > 0 FROM pg_stat_replication_slots WHERE slot_name = 's'"),
	'the large transaction was streamed');

# truncation
$pub->safe_psql('postgres', 'TRUNCATE tc');
$pub->wait_for_catchup('s');
is($sub->safe_psql('postgres', 'SELECT count(*) FROM tc'), '0', 'truncation replicated');

for my $t ('ti', 'tf', 'tc')
{
	is(iheap_verify($pub, $t), '', "structure checks on the publisher: $t");
	is(iheap_verify($sub, $t), '', "structure checks on the subscriber: $t");
}

$sub->stop;
$pub->stop;
done_testing();
