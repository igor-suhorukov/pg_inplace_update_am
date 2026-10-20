--
-- dmbtree indexes with expressions, predicates and INCLUDE columns take
-- in-place updates of the columns they read (DESIGN.md §21 D39)
--
CREATE TABLE dx (id int PRIMARY KEY, s text, n int, flag bool, inc text,
                 pad text)
  USING iheap WITH (autovacuum_enabled = off, fillfactor = 50);
CREATE INDEX dx_lower ON dx (lower(s));           -- expression
CREATE INDEX dx_part ON dx (n) WHERE flag;        -- predicate
CREATE INDEX dx_inc ON dx (n) INCLUDE (inc);      -- INCLUDE column
SELECT c.relname, a.amname
FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
     JOIN pg_am a ON a.oid = c.relam
WHERE i.indrelid = 'dx'::regclass ORDER BY 1;

INSERT INTO dx SELECT g, 'S' || g, g, g % 2 = 0, 'i' || g, 'p'
FROM generate_series(1, 300) g;
VACUUM dx;
CREATE TEMP TABLE dx_tids AS SELECT id, ctid AS t FROM dx;
SELECT tids, marked FROM dmbtree_index_stats('dx_lower');

-- an expression: a change that keeps lower(s) touches no entry; one that
-- changes it marks the old entry and adds the new one
UPDATE dx SET s = lower(s) WHERE id <= 50;
SELECT tids, marked FROM dmbtree_index_stats('dx_lower');
UPDATE dx SET s = 'X' || s WHERE id BETWEEN 51 AND 100;
SELECT tids, marked FROM dmbtree_index_stats('dx_lower');
-- a predicate: rows leave the index and enter it, and change keys within
UPDATE dx SET flag = NOT flag WHERE id BETWEEN 101 AND 150;
UPDATE dx SET n = n + 1000 WHERE id BETWEEN 151 AND 200 AND flag;
SELECT tids, marked FROM dmbtree_index_stats('dx_part');
-- INCLUDE values: the entry is marked and keeps its values
UPDATE dx SET inc = 'new' || id WHERE id BETWEEN 201 AND 250;
SELECT tids, marked FROM dmbtree_index_stats('dx_inc');
SELECT count(*) FILTER (WHERE dx.ctid = t) AS stayed,
       count(*) FILTER (WHERE dx.ctid <> t) AS moved
FROM dx JOIN dx_tids USING (id);
SELECT pg_stat_force_next_flush();
SELECT inplace_updates, inplace_key_updates, moved_updates
FROM iheap_stat('dx');

-- every scan finds each row through its current entry only
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM dx WHERE lower(s) = 's10';
SELECT id FROM dx WHERE lower(s) = 's10';
SELECT id FROM dx WHERE lower(s) = 'xs60';
SELECT id FROM dx WHERE lower(s) = 's60';
SELECT count(*) FROM dx WHERE lower(s) LIKE 'x%';
EXPLAIN (COSTS OFF) SELECT id FROM dx WHERE n = 102 AND flag;
SELECT id FROM dx WHERE n = 102 AND flag;
SELECT id FROM dx WHERE n = 101 AND flag;
SELECT count(*), min(n), max(n) FROM dx WHERE flag AND n > 1000;
SELECT count(*) FROM dx WHERE flag;
-- index-only scans return the row's INCLUDE values, not the marked entry's
EXPLAIN (COSTS OFF) SELECT n, inc FROM dx WHERE n BETWEEN 201 AND 203;
SELECT n, inc FROM dx WHERE n BETWEEN 201 AND 203;
SELECT count(*) FROM dx WHERE n BETWEEN 201 AND 250 AND inc LIKE 'new%';
RESET enable_bitmapscan;
SET enable_indexscan = off;
SELECT count(*) FROM dx WHERE lower(s) IN ('xs60', 's60', 's10');
SELECT count(*) FROM dx WHERE flag AND n BETWEEN 100 AND 120;
RESET enable_indexscan;

-- with the marks kept and the pages all-visible (VACUUM without index
-- cleanup), index-only scans skip the table except for marked entries
VACUUM (INDEX_CLEANUP off) dx;
SELECT tids, marked FROM dmbtree_index_stats('dx_inc');
SET enable_bitmapscan = off;
SELECT n, inc FROM dx WHERE n BETWEEN 201 AND 203;
SELECT n, inc FROM dx WHERE n BETWEEN 251 AND 252;
RESET enable_bitmapscan;

-- the structure checks agree, marks too
SELECT * FROM iheap_check('dx');
SELECT dmbtree_check('dx_lower', true, false, true),
       dmbtree_check('dx_part', true, false, true),
       dmbtree_check('dx_inc', true, false, true);

-- VACUUM removes the old entries, clears the marks of the new ones, and
-- gives entries of the INCLUDE index their rows' values
VACUUM dx;
SELECT tids, marked FROM dmbtree_index_stats('dx_lower');
SELECT tids, marked FROM dmbtree_index_stats('dx_part');
SELECT tids, marked FROM dmbtree_index_stats('dx_inc');
SELECT dmbtree_check('dx_lower', true, false, true),
       dmbtree_check('dx_part', true, false, true),
       dmbtree_check('dx_inc', true, false, true);
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT n, inc FROM dx WHERE n BETWEEN 201 AND 203;
SELECT id FROM dx WHERE lower(s) = 'xs60';
RESET enable_bitmapscan;
RESET enable_seqscan;

-- an INCLUDE value too large for an entry is refused, as when the row moves
UPDATE dx SET inc = (SELECT string_agg(md5(g::text), '') FROM generate_series(1, 100) g)
WHERE id = 300;
UPDATE dx SET inc = (SELECT string_agg(md5(g::text), '') FROM generate_series(1, 400) g)
WHERE id = 300;

-- uniqueness of an expression, and of a partial index (unique indexes are
-- btree by default: iheap.unique_index_am)
CREATE UNIQUE INDEX dx_ulower ON dx USING dmbtree (lower(s));
UPDATE dx SET s = upper(s) WHERE id = 3;          -- same key: in place
UPDATE dx SET s = 's2' WHERE id = 1;              -- another row has it
UPDATE dx SET s = 'S1001' WHERE id = 1;
SELECT id, s FROM dx WHERE lower(s) IN ('s1', 's1001', 's2', 's3') ORDER BY id;
SELECT dmbtree_check('dx_ulower', true, true, true);
DROP INDEX dx_ulower;
CREATE UNIQUE INDEX dx_upart ON dx USING dmbtree (n) WHERE flag;
UPDATE dx SET n = 4 WHERE id = 3;                 -- row 3 is out of the index
UPDATE dx SET flag = true WHERE id = 3;           -- now it enters it: conflict
UPDATE dx SET n = 3 WHERE id = 3;
UPDATE dx SET flag = true WHERE id = 3;
SELECT id, n, flag FROM dx WHERE id IN (3, 4) ORDER BY id;
SELECT dmbtree_check('dx_upart', true, true, true);

-- an expression that reads values stored out of line
CREATE TABLE dxt (id int PRIMARY KEY, big text) USING iheap;
ALTER TABLE dxt ALTER COLUMN big SET STORAGE EXTERNAL;
CREATE INDEX dxt_len ON dxt (length(big));
INSERT INTO dxt SELECT g, repeat(md5(g::text), 100) FROM generate_series(1, 20) g;
UPDATE dxt SET big = big || 'x' WHERE id <= 10;
SET enable_seqscan = off;
SELECT count(*) FROM dxt WHERE length(big) = 3201;
SELECT count(*) FROM dxt WHERE length(big) = 3200;
RESET enable_seqscan;
SELECT dmbtree_check('dxt_len', true, false, true);
VACUUM dxt;
SELECT tids, marked FROM dmbtree_index_stats('dxt_len');

DROP TABLE dx, dxt;
