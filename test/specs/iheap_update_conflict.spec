# Concurrent updates of the same row: waiting, re-evaluation in READ
# COMMITTED (EvalPlanQual), serialization failures in REPEATABLE READ, and
# rows that moved to a new TID while we waited.

setup
{
  CREATE TABLE uc (id int PRIMARY KEY, k int UNIQUE, v int, note text) USING iheap;
  INSERT INTO uc VALUES (1, 10, 100, 'a'), (2, 20, 200, 'b');
}

teardown
{
  DROP TABLE uc;
}

session s1
step s1b    { BEGIN; }
step s1u    { UPDATE uc SET v = v + 1 WHERE id = 1; }
step s1k    { UPDATE uc SET k = k + 1 WHERE id = 1; }
step s1grow { UPDATE uc SET note = repeat('x', 500) WHERE id = 1; }
step s1d    { DELETE FROM uc WHERE id = 1; }
step s1c    { COMMIT; }
step s1a    { ROLLBACK; }

session s2
step s2rr   { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM uc; }
step s2u    { UPDATE uc SET v = v * 2 WHERE id = 1; }
step s2uk   { UPDATE uc SET v = v * 2 WHERE k = 10; }
step s2c    { COMMIT; }
step s2sel  { SELECT id, k, v, length(note) AS notelen FROM uc ORDER BY id; }

session s3
step s3u    { UPDATE uc SET v = v + 1000 WHERE id = 1; }
step s3uk   { UPDATE uc SET v = v + 1000 WHERE k = 11; }

# in place, then a waiting in-place update: both apply, in order
permutation s1b s1u s2u s1c s2sel
# two waiters: each re-evaluates against the other's committed result
permutation s1b s1u s2u s3u s1c s2sel
permutation s1b s1k s2u s3uk s1c s2sel
# the first updater rolls back: the second applies to the original row
permutation s1b s1u s2u s1a s2sel
# the row grows in place while the second updater waits
permutation s1b s1grow s2u s1c s2sel
# the key changes (the row moves): the waiter follows it, then rechecks
permutation s1b s1k s2u s1c s2sel
permutation s1b s1k s2uk s1c s2sel
# deleted while waiting: nothing to update
permutation s1b s1d s2u s1c s2sel
# REPEATABLE READ: a concurrent committed update is a serialization failure
permutation s2rr s1b s1u s1c s2u s2c s2sel
permutation s2rr s1b s1u s2u s1c s2c s2sel
permutation s2rr s1b s1u s2u s1a s2c s2sel
