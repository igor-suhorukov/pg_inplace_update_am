# VACUUM FULL / CLUSTER keep the versions that older snapshots still need.

setup
{
  CREATE TABLE rw (id int PRIMARY KEY, v int) USING iheap;
  INSERT INTO rw SELECT g, g FROM generate_series(1, 4) g;
  CREATE TABLE other (x int);
}

teardown
{
  DROP TABLE rw, other;
}

session s1
step s1rr   { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM other; }
step s1sel  { SELECT * FROM rw ORDER BY id; }
step s1c    { COMMIT; }

session s2
step s2upd  { UPDATE rw SET v = v * 10 WHERE id <= 2; UPDATE rw SET v = v + 1 WHERE id = 1; UPDATE rw SET v = v + 1 WHERE id = 1; UPDATE rw SET v = v + 1 WHERE id = 1; UPDATE rw SET v = v + 1 WHERE id = 1; UPDATE rw SET v = v + 1 WHERE id = 1; }
step s2del  { DELETE FROM rw WHERE id = 3; }
step s2ins  { INSERT INTO rw VALUES (5, 5); }
step s2full { VACUUM FULL rw; }
step s2clu  { CLUSTER rw USING rw_pkey; }
step s2sel  { SELECT * FROM rw ORDER BY id; }

permutation s1rr s2upd s2del s2ins s2full s1sel s2sel s1c s1sel
permutation s1rr s2upd s2del s2ins s2clu s1sel s2sel s1c s1sel
