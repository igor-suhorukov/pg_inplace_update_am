# Aborted changes left to lazy rollback (abort callback skipped) are
# invisible to readers and rolled back by the next writer or VACUUM.

setup
{
  CREATE TABLE lr (id int PRIMARY KEY, v int, note text) USING iheap;
  INSERT INTO lr SELECT g, g, 'n' || g FROM generate_series(1, 10) g;
}

teardown
{
  DROP TABLE lr;
}

session s1
setup       { SET iheap.foreground_undo_limit = 0; }
step s1b    { BEGIN; }
step s1upd  { UPDATE lr SET v = v + 100, note = repeat('z', 200); DELETE FROM lr WHERE id > 8; INSERT INTO lr VALUES (11, 11, 'new'); UPDATE lr SET id = id + 50 WHERE id = 1; }
step s1a    { ROLLBACK; }

session s2
step s2sum  { SELECT count(*), sum(v), max(length(note)), max(id) FROM lr; }
step s2upd  { UPDATE lr SET v = v + 1 WHERE id <= 3; }
step s2ins  { INSERT INTO lr VALUES (11, 1100, 'again'); }
step s2vac  { VACUUM lr; }

permutation s1b s1upd s2sum s1a s2sum s2upd s2sum s2ins s2vac s2sum
