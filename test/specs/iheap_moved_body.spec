# Deleted and moved rows keep their bodies on the page while an older
# snapshot may read them (DESIGN.md §21 D40): VACUUM leaves them until the
# snapshot is gone.  Rows with compact headers (§21 D43) put their bodies in
# undo when deleted or moved, where old snapshots find them.

setup
{
  CREATE TABLE mb (id int PRIMARY KEY, k int, v int, pad text) USING iheap;
  CREATE INDEX mb_k ON mb USING btree (k);
  INSERT INTO mb SELECT g, g, g * 10, repeat('p', 20) FROM generate_series(1, 6) g;
  CREATE TABLE other (x int);
}

teardown
{
  DROP TABLE mb, other;
}

session s1
step s1rr    { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM other; }
step s1sel   { SELECT id, k, v FROM mb ORDER BY id; }
step s1idx   { SET enable_seqscan = off; SET enable_bitmapscan = off; SELECT id, k, v FROM mb WHERE k < 1000 ORDER BY k; RESET enable_seqscan; RESET enable_bitmapscan; }
step s1c     { COMMIT; }

session s2
step s2move  { UPDATE mb SET k = k + 1000 WHERE id <= 2; }
step s2del   { DELETE FROM mb WHERE id = 3; }
step s2vac   { VACUUM mb; }
step s2stat  { SELECT deleted_items, stub_items, dead_items FROM iheap_relation_stats('mb'); }
step s2always { SET iheap.compact_headers = always; }
step s2full  { VACUUM FULL mb; }
step s2flags { SELECT count(*) FILTER (WHERE 'COMPACT' = ANY (flags)) AS compact FROM iheap_page_items('mb', 1); }
step s2cmove { UPDATE mb SET k = k + 2000 WHERE id = 4; DELETE FROM mb WHERE id = 5; }
step s2sel   { SELECT id, k, v FROM mb ORDER BY id; }
step s2check { SELECT * FROM iheap_check('mb'); }

permutation s1rr s2move s2del s2vac s2stat s1sel s1idx s1c s2vac s2stat s2sel s2check
permutation s2always s2full s2flags s1rr s2cmove s2vac s2stat s1sel s1idx s2sel s2check s1c s2vac s2stat s2check
