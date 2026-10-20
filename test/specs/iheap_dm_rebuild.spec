# A dmbtree index built while an old snapshot sees older keys of rows that
# in-place updates changed -- rebuilt by REINDEX, VACUUM FULL or CLUSTER, or
# created anew -- gets entries for those keys too, delete-marked (DESIGN.md
# §21 D32 B): the old snapshot uses it at once (no indcheckxmin) and finds
# the rows by the keys it sees, also through expressions and predicates, and
# index-only scans return the INCLUDE values its versions have (§21 D39).
# VACUUM removes the older keys' entries once the snapshot is gone.

setup
{
  CREATE TABLE dr (id int PRIMARY KEY, k int, s text, flag bool, inc int)
    USING iheap WITH (autovacuum_enabled = off);
  CREATE INDEX dr_k ON dr USING dmbtree (k);
  CREATE INDEX dr_lower ON dr USING dmbtree (lower(s));
  CREATE INDEX dr_part ON dr USING dmbtree (k) WHERE flag;
  CREATE INDEX dr_inc ON dr USING dmbtree (id) INCLUDE (inc);
  INSERT INTO dr SELECT g, g, 'S' || g, true, g FROM generate_series(1, 6) g;
  CREATE TABLE other (x int);
}
setup
{
  VACUUM dr;
}

teardown
{
  DROP TABLE dr, other;
}

session s1
setup		{ SET enable_seqscan = off; SET enable_bitmapscan = off; }
step s1rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM other; }
step s1plan	{ EXPLAIN (COSTS OFF) SELECT id, k FROM dr WHERE k BETWEEN 1 AND 6; }
step s1k	{ SELECT id, k FROM dr WHERE k BETWEEN 1 AND 6 ORDER BY k; }
step s1lower	{ SELECT id FROM dr WHERE lower(s) IN ('s1', 's2', 's3', 'xs1') ORDER BY id; }
step s1part	{ SELECT id FROM dr WHERE flag AND k BETWEEN 1 AND 6 ORDER BY id; }
step s1incplan	{ EXPLAIN (COSTS OFF) SELECT id, inc FROM dr WHERE id BETWEEN 1 AND 3; }
step s1inc	{ SELECT id, inc FROM dr WHERE id BETWEEN 1 AND 3 ORDER BY id; }
step s1c	{ COMMIT; }

session s2
setup		{ SET enable_seqscan = off; SET enable_bitmapscan = off; }
step s2upd	{ UPDATE dr SET k = k + 100, s = 'X' || s, inc = inc + 1000 WHERE id <= 3;
			  UPDATE dr SET flag = false WHERE id = 4; }
step s2reindex	{ REINDEX TABLE dr; }
step s2reindexpar	{ ALTER TABLE dr SET (parallel_workers = 2);
			  SET max_parallel_maintenance_workers = 2;
			  SET min_parallel_table_scan_size = 0;
			  REINDEX TABLE dr;
			  RESET max_parallel_maintenance_workers;
			  RESET min_parallel_table_scan_size; }
step s2vacfull	{ VACUUM FULL dr; }
step s2cluster	{ CLUSTER dr USING dr_pkey; }
step s2create	{ DROP INDEX dr_k; CREATE INDEX dr_k ON dr USING dmbtree (k); }
step s2xmin	{ SELECT c.relname, i.indcheckxmin
			  FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
			  WHERE i.indrelid = 'dr'::regclass ORDER BY 1; }
step s2marks	{ SELECT 'dr_k' AS idx, * FROM dmbtree_index_stats('dr_k')
			  UNION ALL SELECT 'dr_lower', * FROM dmbtree_index_stats('dr_lower')
			  UNION ALL SELECT 'dr_part', * FROM dmbtree_index_stats('dr_part')
			  UNION ALL SELECT 'dr_inc', * FROM dmbtree_index_stats('dr_inc'); }
step s2check	{ SELECT dmbtree_check('dr_k', true, false, true),
				 dmbtree_check('dr_lower', true, false, true),
				 dmbtree_check('dr_part', true, false, true),
				 dmbtree_check('dr_inc', true, false, true); }
step s2sel	{ SELECT id, k, s, flag, inc FROM dr ORDER BY id; }
step s2vac	{ VACUUM dr; }

permutation s1rr s2upd s2reindex s2xmin s2marks s2check s1plan s1k s1lower s1part s1incplan s1inc s1c s2vac s2marks s2sel
permutation s1rr s2upd s2vacfull s2xmin s1k s1lower s1part s1inc s2check s1c s2vac s2marks
permutation s1rr s2upd s2cluster s2xmin s1k s1lower s1part s1inc s1c s2vac s2marks
permutation s1rr s2upd s2create s2xmin s1plan s1k s1c s2vac s2marks
# the same rebuild with parallel workers, which spool the marked entries too
permutation s1rr s2upd s2reindexpar s2xmin s2marks s1k s1lower s1part s1inc s2check s1c s2vac s2marks
