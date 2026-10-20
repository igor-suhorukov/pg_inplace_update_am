# Old snapshots read prior versions out of undo: after in-place updates,
# growth, moves and deletes by committed transactions, and while VACUUM runs.

setup
{
  CREATE TABLE os (id int PRIMARY KEY, k int, v int, note text) USING iheap;
  CREATE INDEX os_k ON os (k);
  INSERT INTO os SELECT g, g, g * 10, 'n' || g FROM generate_series(1, 5) g;
  CREATE TABLE other (x int);
}

teardown
{
  DROP TABLE os, other;
}

session s1
step s1rr   { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM other; }
step s1sel  { SELECT id, k, v, note FROM os ORDER BY id; }
step s1idx  { SET enable_seqscan = off; SELECT id, k, v FROM os WHERE k BETWEEN 1 AND 5 ORDER BY k; RESET enable_seqscan; }
step s1c    { COMMIT; }

session s2
step s2upd  { UPDATE os SET v = v + 1; UPDATE os SET v = v + 1 WHERE id <= 2; }
step s2grow { UPDATE os SET note = repeat('g', 300) WHERE id = 3; }
step s2key  { UPDATE os SET k = k + 100 WHERE id = 4; }
step s2del  { DELETE FROM os WHERE id = 5; }
step s2ins  { INSERT INTO os VALUES (6, 6, 60, 'new'); }
step s2vac  { VACUUM os; }
step s2sel  { SELECT id, k, v, left(note, 5) AS note FROM os ORDER BY id; }
step s2disc { SELECT live_undo_pages = 0 AS discarded FROM iheap_metapage_info('os'); }

permutation s1rr s2upd s2grow s2key s2del s2ins s1sel s1idx s2vac s1sel s2sel s1c s2vac s2disc s2sel
