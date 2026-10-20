# Row locks: which lock modes conflict with which modifications.

setup
{
  CREATE TABLE rl (id int PRIMARY KEY, v int) USING iheap;
  INSERT INTO rl VALUES (1, 1), (2, 2), (3, 3);
}

teardown
{
  DROP TABLE rl;
}

session s1
step s1b    { BEGIN; }
step s1ks   { SELECT id FROM rl WHERE id = 1 FOR KEY SHARE; }
step s1sh   { SELECT id FROM rl WHERE id = 1 FOR SHARE; }
step s1nku  { SELECT id FROM rl WHERE id = 1 FOR NO KEY UPDATE; }
step s1fu   { SELECT id FROM rl WHERE id = 1 FOR UPDATE; }
step s1c    { COMMIT; }

session s2
step s2b    { BEGIN; }
step s2upd  { UPDATE rl SET v = v + 10 WHERE id = 1; }
step s2key  { UPDATE rl SET id = 11 WHERE id = 1; }
step s2ks   { SELECT id FROM rl WHERE id = 1 FOR KEY SHARE; }
step s2now  { SELECT id FROM rl WHERE id = 1 FOR UPDATE NOWAIT; }
step s2skip { SELECT id FROM rl ORDER BY id FOR UPDATE SKIP LOCKED; }
step s2c    { COMMIT; }

session s3
step s3sel  { SELECT * FROM rl ORDER BY id; }

# KEY SHARE does not block a non-key update, blocks a key update
permutation s1b s1ks s2upd s1c s3sel
permutation s1b s1ks s2key s1c s3sel
# SHARE blocks any update
permutation s1b s1sh s2upd s1c s3sel
# KEY SHARE locks of two sessions coexist; both then see the row
permutation s1b s1ks s2b s2ks s1c s2c s3sel
# NO KEY UPDATE blocks a non-key update but not KEY SHARE
permutation s1b s1nku s2ks s1c s3sel
permutation s1b s1nku s2upd s1c s3sel
# NOWAIT and SKIP LOCKED
permutation s1b s1fu s2now s1c
permutation s1b s1fu s2b s2skip s2c s1c
