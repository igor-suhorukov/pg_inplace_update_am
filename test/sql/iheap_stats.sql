--
-- Cumulative statistics of iheap relations (iheap_stat, DESIGN.md §15)
--
-- A backend flushes its counts when it goes idle, at most once a second;
-- pg_stat_force_next_flush() makes the next flush happen at once, and the
-- next statement reads them.  The tables have autovacuum off: only this
-- session touches them.

CREATE TABLE st (id int PRIMARY KEY, k int, v int, s text)
  USING iheap WITH (autovacuum_enabled = off);

-- nothing counted yet
SELECT inplace_updates, moved_updates, undo_records, undo_walks,
       stats_reset IS NULL AS never_reset
FROM iheap_stat('st');

-- one undo record per row inserted
INSERT INTO st SELECT g, g, g, 'x' FROM generate_series(1, 10) g;

-- in place: v is in no index
UPDATE st SET v = v + 1 WHERE id <= 4;

-- in place, changing the key of a dmbtree index (the default index AM)
CREATE INDEX st_k ON st (k);
UPDATE st SET k = k + 100 WHERE id <= 3;

-- moves: a column of a btree index changes
CREATE INDEX st_v ON st USING btree (v);
UPDATE st SET v = v + 1 WHERE id = 5;

-- moves: an AFTER ROW UPDATE trigger needs the old row by TID
CREATE FUNCTION st_trig() RETURNS trigger LANGUAGE plpgsql AS
$$ BEGIN RETURN NULL; END $$;
CREATE TRIGGER st_after AFTER UPDATE ON st FOR EACH ROW EXECUTE FUNCTION st_trig();
UPDATE st SET s = 'y' WHERE id = 6;
DROP TRIGGER st_after ON st;

-- moves: in-place updates turned off
SET iheap.enable_inplace = off;
UPDATE st SET s = 'z' WHERE id = 7;
RESET iheap.enable_inplace;

-- a deletion and a row lock write one undo record each
DELETE FROM st WHERE id = 10;
SELECT id FROM st WHERE id = 9 FOR UPDATE;

SELECT pg_stat_force_next_flush();
SELECT inplace_updates, inplace_key_updates, moved_updates, moved_for_index,
       moved_for_trigger, moved_for_size, moved_for_setting, undo_records,
       undo_bytes > 0 AS undo_bytes
FROM iheap_stat('st');

-- moves: the new version does not fit on the page (7 rows of 1 kB fill it)
CREATE TABLE sz (id int, s text) USING iheap WITH (autovacuum_enabled = off);
INSERT INTO sz SELECT g, repeat('a', 1000) FROM generate_series(1, 7) g;
UPDATE sz SET s = repeat('b', 1900) WHERE id = 1;
SELECT pg_stat_force_next_flush();
SELECT inplace_updates, moved_updates, moved_for_size FROM iheap_stat('sz');

--
-- Rollback
--
-- by the aborting backend
BEGIN;
UPDATE st SET s = 'r' WHERE id <= 5;
ROLLBACK;

-- lazily: aborted changes over the limit are left behind; readers walk
-- past them and the next writer of each row applies them
SET iheap.foreground_undo_limit = 0;
BEGIN;
UPDATE st SET s = 'l' WHERE id <= 3;
ROLLBACK;
RESET iheap.foreground_undo_limit;
UPDATE st SET s = 'm' WHERE id <= 3;

-- by discard, which applies what is left before it passes the undo
SET iheap.foreground_undo_limit = 0;
BEGIN;
UPDATE st SET s = 'd' WHERE id = 4;
ROLLBACK;
RESET iheap.foreground_undo_limit;
SELECT iheap_discard('st') > 0 AS discarded;

SELECT pg_stat_force_next_flush();
SELECT inplace_updates, undo_records, rollback_eager, rollback_lazy,
       rollback_discard, rollback_bytes > 0 AS rollback_bytes,
       discarded_pages > 0 AS discarded_pages,
       discarded_bytes > 0 AS discarded_bytes,
       undo_walks, undo_walk_steps, undo_walk_max
FROM iheap_stat('st');
SELECT s FROM st WHERE id <= 5 ORDER BY id;

--
-- Walks through undo: a cursor keeps the snapshot it was declared with
--
SELECT iheap_stat_reset('st');
BEGIN;
DECLARE c CURSOR FOR SELECT id, s FROM st WHERE id <= 4 ORDER BY id;
UPDATE st SET s = s || '+' WHERE id <= 4;
UPDATE st SET s = s || '+' WHERE id <= 2;
FETCH ALL FROM c;
COMMIT;
SELECT pg_stat_force_next_flush();
SELECT inplace_updates, undo_records, undo_walks, undo_walk_steps,
       undo_walk_max, stats_reset IS NOT NULL AS reset
FROM iheap_stat('st');

-- track_counts off: nothing is counted
SET track_counts = off;
UPDATE st SET s = 'off' WHERE id = 1;
RESET track_counts;
SELECT pg_stat_force_next_flush();
SELECT inplace_updates FROM iheap_stat('st');

-- the view lists iheap tables
SELECT relname, inplace_updates, moved_updates, moved_for_size
FROM iheap_stat_tables WHERE relname IN ('st', 'sz') ORDER BY relname;

--
-- An entry goes with its relation, and with the transaction that made it
--
SELECT 'sz'::regclass::oid AS sz_oid \gset
DROP TABLE sz;
SELECT pg_stat_force_next_flush();
SELECT moved_updates, undo_records FROM iheap_stat(:sz_oid);

BEGIN;
CREATE TABLE gone (id int) USING iheap;
INSERT INTO gone VALUES (1);
SELECT 'gone'::regclass::oid AS gone_oid \gset
ROLLBACK;
SELECT pg_stat_force_next_flush();
SELECT undo_records FROM iheap_stat(:gone_oid);

-- resetting everything; only iheap relations can be reset one by one
SELECT iheap_stat_reset();
SELECT pg_stat_force_next_flush();
SELECT inplace_updates, undo_records FROM iheap_stat('st');
SELECT iheap_stat_reset('pg_class');

DROP TABLE st;
DROP FUNCTION st_trig();
