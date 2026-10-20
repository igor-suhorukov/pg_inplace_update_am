# READ COMMITTED rechecks (EvalPlanQual) of rows updated in place.  An
# in-place update keeps the TID, which in heap names one version; the
# rechecks must still see the version they would see in heap.

setup
{
  CREATE TABLE jobs (id int PRIMARY KEY, status text, beat int) USING iheap;
  INSERT INTO jobs VALUES (1, 'new', 0), (2, 'new', 0);
  CREATE TABLE acct (id int PRIMARY KEY, bal int) USING iheap;
  INSERT INTO acct VALUES (1, 100), (2, 200);
}

teardown
{
  DROP TABLE jobs, acct;
}

session s1
step s1b    { BEGIN; }
step s1beat { UPDATE jobs SET beat = beat + 1 WHERE id = 1; }
step s1take { UPDATE jobs SET status = 'done' WHERE id = 1; }
step s1bal  { UPDATE acct SET bal = bal + 1000 WHERE id = 1; }
step s1c    { COMMIT; }

session s2
# a queue consumer: the subquery locks the row, the update must still
# recheck it and keep the concurrent change of "beat"
step s2pick {
  UPDATE jobs SET status = 'taken'
  WHERE id = (SELECT id FROM jobs WHERE status = 'new' ORDER BY id LIMIT 1 FOR UPDATE)
  RETURNING *;
}
# the joined row that is not locked is the one of the original snapshot
step s2join {
  SELECT a1.id, a1.bal, a2.bal AS bal_unlocked
  FROM acct a1 JOIN acct a2 ON a1.id = a2.id
  WHERE a1.id = 1
  FOR UPDATE OF a1;
}
step s2sel  { SELECT * FROM jobs ORDER BY id; }

permutation s1b s1beat s2pick s1c s2sel
# the picked row is taken meanwhile: the recheck moves on to the next one
permutation s1b s1take s2pick s1c s2sel
permutation s1b s1bal s2join s1c
