--
-- Basic DML, in-place updates and the on-page format
--
CREATE EXTENSION IF NOT EXISTS pageinspect;
CREATE TABLE ib (id int PRIMARY KEY, v int, s text) USING iheap;

SELECT amname FROM pg_am a JOIN pg_class c ON c.relam = a.oid
WHERE c.relname = 'ib';

INSERT INTO ib SELECT g, g * 10, 'row ' || g FROM generate_series(1, 10) g;
SELECT * FROM ib ORDER BY id;

-- an update of a non-indexed column stays at the same TID
CREATE TEMP TABLE tids AS SELECT id, ctid AS old_ctid FROM ib;
UPDATE ib SET v = v + 1 WHERE id <= 5;
SELECT i.id, i.v, i.ctid = t.old_ctid AS same_tid
FROM ib i JOIN tids t USING (id) ORDER BY id;

-- growing and shrinking in place
UPDATE ib SET s = s || repeat('+', 200) WHERE id = 6;
SELECT id, length(s), ctid = (SELECT old_ctid FROM tids WHERE id = 6) AS same_tid
FROM ib WHERE id = 6;
UPDATE ib SET s = 'short' WHERE id = 6;
SELECT id, s, ctid = (SELECT old_ctid FROM tids WHERE id = 6) AS same_tid
FROM ib WHERE id = 6;

-- updating an indexed column moves the row (new index entry needed)
UPDATE ib SET id = 100 WHERE id = 7;
SELECT id, ctid = (SELECT old_ctid FROM tids WHERE id = 7) AS same_tid
FROM ib WHERE id = 100;
SELECT count(*) FROM ib WHERE id = 7;

-- NULLs, setting to and from NULL
UPDATE ib SET v = NULL WHERE id = 8;
UPDATE ib SET s = NULL WHERE id = 9;
SELECT * FROM ib WHERE id IN (8, 9) ORDER BY id;
UPDATE ib SET v = 81, s = 'back' WHERE id = 8;
SELECT * FROM ib WHERE id = 8;

DELETE FROM ib WHERE id IN (2, 4);
SELECT id FROM ib ORDER BY id;

-- several updates of a row in one transaction
BEGIN;
UPDATE ib SET v = 1000 WHERE id = 1;
UPDATE ib SET v = v + 1 WHERE id = 1;
UPDATE ib SET s = 'changed' WHERE id = 1;
SELECT * FROM ib WHERE id = 1;
COMMIT;
SELECT * FROM ib WHERE id = 1;

-- system columns
SELECT xmin = xmax AS no, cmin, cmax FROM ib WHERE id = 1;
BEGIN;
UPDATE ib SET v = v WHERE id = 3;
SELECT xmin = pg_current_xact_id()::xid AS updated_by_us FROM ib WHERE id = 3;
COMMIT;

-- the 16-byte header, unaligned pass-by-value attributes
CREATE TABLE ifmt (a int2, b int8, c bool, d int4, e float8) USING iheap;
INSERT INTO ifmt VALUES (1, 2, true, 3, 4.5);
SELECT lp, lp_len, natts, hoff, flags FROM iheap_page_items('ifmt', 1);
SELECT * FROM ifmt;
CREATE TABLE hfmt (a int2, b int8, c bool, d int4, e float8);
INSERT INTO hfmt VALUES (1, 2, true, 3, 4.5);
SELECT lp_len AS heap_lp_len FROM heap_page_items(get_raw_page('hfmt', 0));

-- pass-by-reference values after unaligned ones
CREATE TABLE imix (a int2, n numeric, b int8, t text, ts timestamptz, i interval,
                   arr int[], c char(3)) USING iheap;
INSERT INTO imix VALUES (7, 3.14159, 1234567890123, 'text',
                         '2026-01-02 03:04:05+00', '1 day 2 hours', '{1,2,3}', 'ab');
INSERT INTO imix VALUES (NULL, NULL, 5, NULL, NULL, '3 years', NULL, 'xyz');
SELECT * FROM imix ORDER BY b;
UPDATE imix SET n = n * 2, arr = arr || 4 WHERE a = 7;
SELECT * FROM imix ORDER BY b;

-- many columns, wide null bitmap
SELECT 'CREATE TABLE iwide (' ||
       string_agg('c' || g || ' int', ', ') || ') USING iheap'
FROM generate_series(1, 100) g \gexec
INSERT INTO iwide (c1, c50, c100) VALUES (1, 50, 100);
SELECT c1, c2, c50, c99, c100 FROM iwide;
UPDATE iwide SET c2 = 2, c99 = 99;
SELECT c1, c2, c50, c99, c100 FROM iwide;

-- empty and zero-column tables
CREATE TABLE izero () USING iheap;
INSERT INTO izero DEFAULT VALUES;
INSERT INTO izero DEFAULT VALUES;
SELECT count(*) FROM izero;
DELETE FROM izero;
SELECT count(*) FROM izero;

-- TID range scans, serial and parallel, bounded and unbounded
CREATE TABLE itid (id int, pad text) USING iheap WITH (fillfactor = 10);
INSERT INTO itid SELECT g, repeat('x', 100) FROM generate_series(1, 300) g;
SELECT count(*) FROM itid WHERE ctid > '(3,0)';
SELECT count(*) FROM itid WHERE ctid < '(20,1)';
SELECT count(*) FROM itid WHERE ctid > '(3,0)' AND ctid < '(20,1)';
SELECT count(*) FROM itid WHERE ctid > '(100000,0)';
BEGIN;
SET LOCAL parallel_setup_cost = 0;
SET LOCAL parallel_tuple_cost = 0;
SET LOCAL min_parallel_table_scan_size = 0;
SET LOCAL max_parallel_workers_per_gather = 4;
EXPLAIN (COSTS OFF) SELECT count(*) FROM itid WHERE ctid > '(3,0)';
SELECT count(*) FROM itid WHERE ctid > '(3,0)';
SELECT count(*) FROM itid WHERE ctid < '(20,1)';
SELECT count(*) FROM itid WHERE ctid > '(3,0)' AND ctid < '(20,1)';
SELECT count(*) FROM itid WHERE ctid > '(100000,0)';
COMMIT;
DROP TABLE itid;

-- inspection functions
SELECT nextents >= 1 AS has_undo, live_undo_pages >= 0 AS ok
FROM iheap_metapage_info('ib');
SELECT nblocks > 0, data_pages, live_items FROM iheap_relation_stats('ib');
SELECT iheap_page_items('ib', 0);  -- metapage: error
SELECT iheap_page_items('tids', 0);  -- not iheap: error

DROP TABLE ib, ifmt, hfmt, imix, iwide, izero;
