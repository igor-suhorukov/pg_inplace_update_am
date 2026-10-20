# A transaction updates a row in place, then moves it (indexed column), and
# rolls back while other writers wait for it.  The waiters must build their
# new versions from the restored row, never from the aborted versions.

setup
{
  CREATE TABLE ac (id int PRIMARY KEY, branch int, balance int, note text) USING iheap;
  CREATE INDEX ac_branch ON ac (branch);
  INSERT INTO ac VALUES (1, 1, 1000, 'n'), (2, 2, 1000, 'n');
}

teardown
{
  DROP TABLE ac;
}

session a
step ab  { BEGIN; }
step au  { UPDATE ac SET balance = balance + 1000000 WHERE id = 1; }
step am  { UPDATE ac SET branch = branch + 100 WHERE id = 1; }
step ar  { ROLLBACK; }
step ac  { COMMIT; }

session m
step mb  { BEGIN; }
step mm  { UPDATE ac SET branch = 5, note = repeat('m', 50) WHERE id = 1; }
step ms  { UPDATE ac SET branch = branch, note = repeat('s', 80) WHERE id = 1; }
step mc  { COMMIT; }

session t
step tu  { UPDATE ac SET balance = balance + 1 WHERE id = 1; }
step tsel { SELECT id, branch, balance, length(note) AS notelen FROM ac ORDER BY id; }

# a waiting move / in-place resize while the other transaction rolls back
permutation ab au am mm ar tsel
permutation ab au mm am ar tsel
permutation ab au am ms ar tsel
# two waiters behind the aborting transaction
permutation ab au am mm tu ar tsel
# the same, but committing: the waiters re-evaluate on the moved row
permutation ab au am mm ac tsel
permutation ab au am tu ac tsel
# the mover first, then the in-place updater re-evaluates after it commits
permutation mb mm ab au mc am ar tsel
permutation mb ms ab au mc am ar tsel
