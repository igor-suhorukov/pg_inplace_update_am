# Old snapshots and dmbtree indexes: rows whose keys an in-place update
# changed are found through the keys the snapshot sees -- by index,
# index-only and bitmap scans -- also after a key comes back and while
# VACUUM cleans up entries nobody else needs.  An index rebuilt meanwhile
# (REINDEX, VACUUM FULL) has the newest keys only, and the old snapshot
# does not use it.

setup
{
  CREATE TABLE ds (id int PRIMARY KEY, k int, v int) USING iheap
    WITH (autovacuum_enabled = off);
  CREATE INDEX ds_k ON ds USING dmbtree (k);
  INSERT INTO ds SELECT g, g, 0 FROM generate_series(1, 6) g;
  CREATE TABLE other (x int);
}

teardown
{
  DROP TABLE ds, other;
}

session s1
setup		{ SET enable_seqscan = off; }
step s1rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM other; }
step s1idx	{ SET enable_bitmapscan = off; SELECT id, k FROM ds WHERE k BETWEEN 1 AND 6 ORDER BY k; }
step s1new	{ SET enable_bitmapscan = off; SELECT id, k FROM ds WHERE k >= 100 ORDER BY k; }
step s1ios	{ SET enable_bitmapscan = off; SELECT k FROM ds WHERE k <= 1000 ORDER BY k; }
step s1bm	{ SET enable_indexscan = off; SET enable_bitmapscan = on; SELECT id, k FROM ds WHERE k IN (1, 2, 101, 102) ORDER BY id; RESET enable_indexscan; }
step s1c	{ COMMIT; }

session s2
setup		{ SET enable_seqscan = off; SET enable_bitmapscan = off; }
step s2key	{ UPDATE ds SET k = k + 100 WHERE id <= 3; }
step s2back	{ UPDATE ds SET k = k - 100 WHERE id = 1; }
step s2vac	{ VACUUM ds; }
step s2reindex	{ REINDEX INDEX ds_k; }
step s2vacfull	{ VACUUM FULL ds; }
step s2sel	{ SELECT id, k FROM ds WHERE k BETWEEN 0 AND 1000 ORDER BY k; }
step s2marks	{ SELECT tids, marked FROM dmbtree_index_stats('ds_k'); }

permutation s1rr s2key s1idx s1new s1ios s1bm s2back s1idx s1bm s2vac s1idx s1ios s2sel s1c s2vac s2marks s2sel
permutation s1rr s2key s2reindex s1idx s1ios s1c s2sel
permutation s1rr s2key s2vacfull s1idx s1ios s1c s2sel
