# Helpers shared by the iheap TAP tests.
package IheapTest;

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Exporter 'import';

our @EXPORT = qw(iheap_node iheap_workload iheap_snapshot iheap_verify);

# A node with the module preloaded and the extension created in "postgres",
# with amcheck and iheap_verify() for structure checks.
sub iheap_node
{
	my ($name, %params) = @_;
	my $extra_conf = $params{extra_conf} // '';
	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init(
		allows_streaming => $params{allows_streaming} // 0,
		no_data_checksums => $params{no_data_checksums} // 0);
	$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = 'pg_inplace_update_am'
max_prepared_transactions = 10
autovacuum = off
$extra_conf
});
	$node->start;
	$node->safe_psql('postgres', 'CREATE EXTENSION pg_inplace_update_am');
	$node->safe_psql('postgres', 'CREATE EXTENSION amcheck');
	$node->safe_psql('postgres', q{
-- every problem iheap_check() and the index checkers find in a table: its
-- dmbtree indexes with every option, its btree ones with amcheck
CREATE FUNCTION iheap_verify(rel regclass) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
  r record;
BEGIN
  FOR r IN SELECT * FROM iheap_check(rel) LOOP
    RETURN NEXT format('%s: block %s, offset %s: %s', rel, r.blkno, r.offnum, r.msg);
  END LOOP;
  FOR r IN
    SELECT i.indexrelid::regclass AS idx, a.amname, i.indisunique
    FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
         JOIN pg_am a ON a.oid = c.relam
    WHERE i.indrelid = rel AND i.indisvalid ORDER BY i.indexrelid::regclass::text
  LOOP
    BEGIN
      IF r.amname = 'dmbtree' THEN
        PERFORM dmbtree_check(r.idx, true, r.indisunique, true);
      ELSIF r.amname = 'btree' THEN
        PERFORM bt_index_check(r.idx, true, r.indisunique);
      END IF;
    EXCEPTION WHEN OTHERS THEN
      RETURN NEXT format('%s: %s', r.idx, SQLERRM);
    END;
  END LOOP;
END $$;
-- the same, as an error (for pgbench scripts)
CREATE FUNCTION iheap_verify_strict(rel regclass) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE
  p text;
BEGIN
  FOR p IN SELECT * FROM iheap_verify(rel) LOOP
    RAISE EXCEPTION 'verification failed: %', p;
  END LOOP;
END $$;
});
	return $node;
}

# Problems the structure checks find in a table, one per line ('' if none).
sub iheap_verify
{
	my ($node, $table, %opts) = @_;
	return $node->safe_psql($opts{db} // 'postgres',
		"SELECT string_agg(p, E'\\n') FROM iheap_verify('$table') p");
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
