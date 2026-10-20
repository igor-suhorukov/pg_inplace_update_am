# Helpers shared by the iheap TAP tests.
package IheapTest;

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Exporter 'import';

our @EXPORT = qw(iheap_node iheap_workload iheap_snapshot);

# A node with the module preloaded and the extension created in "postgres".
sub iheap_node
{
	my ($name, %params) = @_;
	my $extra_conf = $params{extra_conf} // '';
	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init(allows_streaming => $params{allows_streaming} // 0);
	$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = 'pg_inplace_update_am'
max_prepared_transactions = 10
autovacuum = off
$extra_conf
});
	$node->start;
	$node->safe_psql('postgres', 'CREATE EXTENSION pg_inplace_update_am');
	return $node;
}

# Every kind of change, applied identically to an iheap table and a heap
# table so that their contents can be compared afterwards.
sub iheap_workload
{
	my ($node, $round) = @_;
	my $sql = '';
	for my $t ('ti', 'th')
	{
		$sql .= qq{
UPDATE $t SET v = v + 1 WHERE id % 3 = $round % 3;
UPDATE $t SET s = repeat(chr(97 + $round % 26), 10 + (id * $round) % 90) WHERE id % 5 = $round % 5;
UPDATE $t SET k = k + 1000 WHERE id % 17 = $round % 17;
DELETE FROM $t WHERE id % 29 = $round % 29 AND id > 1000;
INSERT INTO $t SELECT g, g, g, 'ins' || g, NULL
  FROM generate_series(1000 + $round * 50 + 1, 1000 + $round * 50 + 50) g;
UPDATE $t SET big = repeat(md5(id::text || '$round'), 200) WHERE id % 50 = $round % 50;
BEGIN; UPDATE $t SET v = -1; DELETE FROM $t WHERE id < 100; ROLLBACK;
BEGIN; SAVEPOINT a; UPDATE $t SET v = v + 1000 WHERE id < 50; ROLLBACK TO a; UPDATE $t SET v = v + 2 WHERE id < 20; COMMIT;
};
	}
	$node->safe_psql('postgres', $sql);
}

# Contents of a table as one string, for comparisons.
sub iheap_snapshot
{
	my ($node, $table, %opts) = @_;
	my $where = $opts{where} // 'true';
	return $node->safe_psql($opts{db} // 'postgres',
		"SELECT count(*), sum(v), sum(k), md5(string_agg(id || ':' || k || ':' || v || ':' || s || ':' || coalesce(md5(big), '-'), ',' ORDER BY id)) FROM $table WHERE $where");
}

sub setup_tables
{
	my ($node) = @_;
	$node->safe_psql('postgres', q{
CREATE TABLE ti (id int PRIMARY KEY, k int, v int, s text, big text) USING iheap;
CREATE INDEX ti_k ON ti (k);
CREATE TABLE th (id int PRIMARY KEY, k int, v int, s text, big text) USING heap;
CREATE INDEX th_k ON th (k);
INSERT INTO ti SELECT g, g, g, 's' || g, NULL FROM generate_series(1, 1000) g;
INSERT INTO th SELECT g, g, g, 's' || g, NULL FROM generate_series(1, 1000) g;
});
}

push @EXPORT, 'setup_tables';

1;
