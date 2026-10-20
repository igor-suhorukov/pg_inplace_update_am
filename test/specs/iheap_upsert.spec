# INSERT ... ON CONFLICT with concurrent inserters of the same key.

setup
{
  CREATE TABLE up (k int PRIMARY KEY, v int, note text) USING iheap;
}

teardown
{
  DROP TABLE up;
}

session s1
step s1b    { BEGIN; }
step s1ins  { INSERT INTO up VALUES (1, 1, 's1') ON CONFLICT (k) DO UPDATE SET v = up.v + excluded.v, note = up.note || '+s1'; }
step s1c    { COMMIT; }
step s1a    { ROLLBACK; }

session s2
step s2ins  { INSERT INTO up VALUES (1, 10, 's2') ON CONFLICT (k) DO UPDATE SET v = up.v + excluded.v, note = up.note || '+s2'; }
step s2nothing { INSERT INTO up VALUES (1, 100, 's2') ON CONFLICT DO NOTHING; }
step s2sel  { SELECT * FROM up; }

permutation s1b s1ins s2ins s1c s2sel
permutation s1b s1ins s2ins s1a s2sel
permutation s1b s1ins s2nothing s1c s2sel
permutation s1b s1ins s2nothing s1a s2sel
