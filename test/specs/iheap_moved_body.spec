# Deleted and moved rows keep their bodies on the page while an older
# snapshot may read them (DESIGN.md §21 D40): VACUUM leaves them until the
# snapshot is gone.  Rows with compact headers (§21 D43) put their bodies in
# undo when deleted or moved, where old snapshots find them, and so do rows
# that move off a page whose bodies a snapshot holds (§21 D45).

setup
{
  CREATE TABLE mb (id int PRIMARY KEY, k int, v int, pad text) USING iheap;
  CREATE INDEX mb_k ON mb USING btree (k);
  INSERT INTO mb SELECT g, g, g * 10, repeat('p', 20) FROM generate_series(1, 6) g;
  CREATE TABLE other (x int);
  CREATE TABLE fp (id int PRIMARY KEY, k int, pad text) USING iheap;
  CREATE INDEX fp_k ON fp USING btree (k);
  INSERT INTO fp SELECT g, g, repeat('q', 100) FROM generate_series(1, 100) g;
}

teardown
{
  DROP TABLE mb, other, fp;
}

session s1
step s1rr    { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM other; }
step s1sel   { SELECT id, k, v FROM mb ORDER BY id; }
step s1idx   { SET enable_seqscan = off; SET enable_bitmapscan = off; SELECT id, k, v FROM mb WHERE k < 1000 ORDER BY k; RESET enable_seqscan; RESET enable_bitmapscan; }
step s1fp    { SELECT id, k FROM fp WHERE id <= 4 ORDER BY id; }
step s1fpidx { SET enable_seqscan = off; SET enable_bitmapscan = off; SELECT id, k FROM fp WHERE k BETWEEN -10 AND 4 ORDER BY k; RESET enable_seqscan; RESET enable_bitmapscan; }
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
step s2fp1   { UPDATE fp SET k = -k WHERE id = 1; }
step s2fp2   { UPDATE fp SET k = -k WHERE id = 2; }
step s2fp3   { UPDATE fp SET k = -k WHERE id = 3; }
step s2fpstat { SELECT deleted_items, stub_items FROM iheap_relation_stats('fp'); }
step s2fp    { SELECT id, k, (ctid::text::point)[0] = 1 AS first_page FROM fp WHERE id <= 4 ORDER BY id; }
step s2fpcheck { SELECT * FROM iheap_check('fp'); }

permutation s1rr s2move s2del s2vac s2stat s1sel s1idx s1c s2vac s2stat s2sel s2check
permutation s2always s2full s2flags s1rr s2cmove s2vac s2stat s1sel s1idx s2sel s2check s1c s2vac s2stat s2check
# rows moving off a full page: the first leaves its body there, the second
# takes it to undo, and the third moves on the page into the room it left
permutation s1rr s2fp1 s2fp2 s2fp3 s2fpstat s1fp s1fpidx s2fp s2fpcheck s1c
