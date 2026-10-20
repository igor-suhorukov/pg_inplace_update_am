# Uniqueness with dmbtree indexes while another transaction changes a key
# in place: a key being changed away from conflicts until the change
# commits, a key being changed to conflicts unless it aborts -- the same
# waits as with heap.  Also upserts and EvalPlanQual over key changes.

setup
{
  CREATE TABLE dq (id int, code int, v int DEFAULT 0) USING iheap;
  CREATE UNIQUE INDEX dq_code ON dq USING dmbtree (code);
  INSERT INTO dq VALUES (1, 10), (2, 20);
}

teardown
{
  DROP TABLE dq;
}

session s1
step s1b	{ BEGIN; }
step s1chg	{ UPDATE dq SET code = 30 WHERE id = 1; }
step s1c	{ COMMIT; }
step s1a	{ ROLLBACK; }

session s2
step s2ins10	{ INSERT INTO dq VALUES (3, 10); }
step s2ins30	{ INSERT INTO dq VALUES (4, 30); }
step s2upd10	{ UPDATE dq SET code = 10 WHERE id = 2; }
step s2ups10	{ INSERT INTO dq VALUES (5, 10) ON CONFLICT (code) DO UPDATE SET v = dq.v + 1; }
step s2epq	{ UPDATE dq SET v = 100 WHERE code = 10; }
step s2sel	{ SELECT id, code, v FROM dq ORDER BY id; }

permutation s1b s1chg s2ins10 s1c s2sel
permutation s1b s1chg s2ins10 s1a s2sel
permutation s1b s1chg s2ins30 s1c s2sel
permutation s1b s1chg s2ins30 s1a s2sel
permutation s1b s1chg s2upd10 s1c s2sel
permutation s1b s1chg s2upd10 s1a s2sel
permutation s1b s1chg s2ups10 s1c s2sel
permutation s1b s1chg s2ups10 s1a s2sel
permutation s1b s1chg s2epq s1c s2sel
permutation s1b s1chg s2epq s1a s2sel
