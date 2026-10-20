# SERIALIZABLE and dmbtree: an index-only scan passes over a delete-marked
# entry whose row its snapshot sees with another key, without visiting the
# row.  It must still record the rw-conflict with the transaction whose
# change of the row it does not see: below, s1 looks for key 100 just after
# s2 gave row 1 that key, s2 looks for 200, which s1 then gives row 2, and
# one of them must fail (write skew).

setup
{
  CREATE TABLE dz (id int PRIMARY KEY, k int) USING iheap
    WITH (autovacuum_enabled = off);
  CREATE INDEX dz_k ON dz USING dmbtree (k);
  INSERT INTO dz SELECT g, g FROM generate_series(1, 10) g;
  CREATE TABLE other (x int);
}
setup		{ VACUUM ANALYZE dz; }

teardown
{
  DROP TABLE dz, other;
}

session s1
setup		{ SET enable_seqscan = off; SET enable_bitmapscan = off; }
step s1b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT count(*) FROM other; }
step s1r	{ SELECT count(*) FROM dz WHERE k = 100; }
step s1w	{ UPDATE dz SET k = 200 WHERE id = 2; }
step s1c	{ COMMIT; }

session s2
setup		{ SET enable_seqscan = off; SET enable_bitmapscan = off; }
step s2b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT count(*) FROM other; }
step s2w	{ UPDATE dz SET k = 100 WHERE id = 1; }
step s2r	{ SELECT count(*) FROM dz WHERE k = 200; }
step s2c	{ COMMIT; }

session s3
step s3plan	{ SET enable_seqscan = off; SET enable_bitmapscan = off;
			  EXPLAIN (COSTS OFF) SELECT count(*) FROM dz WHERE k = 100; }
step s3sel	{ SELECT id, k FROM dz WHERE id <= 2 ORDER BY id; }

permutation s3plan s1b s2b s2w s1r s2r s1w s1c s2c s3sel
