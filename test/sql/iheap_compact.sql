--
-- Compact tuple headers (DESIGN.md §21 D43): rows written frozen by table
-- rewrites and frozen loads may carry a 12-byte header without xid;
-- iheap.compact_headers decides where.
--
CREATE TABLE ct (id int PRIMARY KEY, a int, s text)
  USING iheap WITH (autovacuum_enabled = off);
INSERT INTO ct SELECT g, g, 'row ' || g FROM generate_series(1, 2000) g;
SELECT data_pages AS pages0 FROM iheap_relation_stats('ct') \gset

-- "auto" (the default) with fillfactor 100: no room for the headers to grow
-- back on an update, so a rewrite keeps them full
SHOW iheap.compact_headers;
VACUUM FULL ct;
SELECT count(*) FILTER (WHERE 'COMPACT' = ANY (flags)) AS compact
FROM iheap_page_items('ct', 1);

-- "always": every row the rewrite writes frozen gets a compact header
SET iheap.compact_headers = always;
VACUUM FULL ct;
SELECT lp, lp_len, xid, urec, frozen, natts, hoff, flags
FROM iheap_page_items('ct', 1) WHERE lp <= 3;
SELECT data_pages < :pages0 AS denser FROM iheap_relation_stats('ct');
SELECT count(*), sum(a), min(s), max(s) FROM ct;
SELECT * FROM iheap_check('ct');

-- a row lock keeps the header compact: it only sets the undo pointer
BEGIN;
SELECT id FROM ct WHERE id = 1 FOR UPDATE;
SELECT lp_len, hoff, flags FROM iheap_page_items('ct', 1) WHERE lp = 1;
COMMIT;
SELECT * FROM iheap_check('ct');
SELECT id FROM ct WHERE id = 2 FOR KEY SHARE;

-- an update gives the new version a full header, 4 bytes more: in place
-- while the page can make room for them (by compaction), moved after that
CREATE TEMP TABLE ct_tids AS SELECT id, ctid AS t FROM ct WHERE id <= 30;
UPDATE ct SET a = a + 1 WHERE id <= 30;
SELECT count(*) FILTER (WHERE ct.ctid = t) AS in_place, count(*) AS updated
FROM ct JOIN ct_tids USING (id);
SELECT lp_len, hoff, flags FROM iheap_page_items('ct', 1) WHERE lp = 3;
SELECT * FROM iheap_check('ct');

-- rolled back: the compact version comes back
BEGIN;
UPDATE ct SET a = -a, s = s || '!' WHERE id BETWEEN 31 AND 35;
ROLLBACK;
SELECT id, a, s FROM ct WHERE id BETWEEN 31 AND 35 ORDER BY id;
SELECT count(*) FILTER (WHERE 'COMPACT' = ANY (flags)) AS compact
FROM iheap_page_items('ct', 1) WHERE lp BETWEEN 31 AND 35;

-- a deletion has no room for its xid: the row's body goes to undo and its
-- item becomes a stub at once; a rollback puts the compact row back
BEGIN;
DELETE FROM ct WHERE id BETWEEN 36 AND 40;
SELECT lp_len, hoff, flags FROM iheap_page_items('ct', 1) WHERE lp = 36;
SELECT * FROM iheap_check('ct');
ROLLBACK;
SELECT id, a, s FROM ct WHERE id BETWEEN 36 AND 40 ORDER BY id;
SELECT lp_len, hoff, flags FROM iheap_page_items('ct', 1) WHERE lp = 36;
DELETE FROM ct WHERE id BETWEEN 41 AND 45;
SELECT count(*) FROM ct WHERE id BETWEEN 41 AND 45;
SELECT * FROM iheap_check('ct');

-- a move (a column of a btree index changes) the same
CREATE INDEX ct_a ON ct USING btree (a);
UPDATE ct SET a = a + 100000 WHERE id BETWEEN 46 AND 50;
SELECT id, a FROM ct WHERE id BETWEEN 46 AND 50 ORDER BY id;
SELECT * FROM iheap_check('ct');
VACUUM ct;
SELECT count(*), sum(a) FROM ct;
SELECT * FROM iheap_check('ct');

-- frozen loads: COPY FREEZE into a table created in the transaction
BEGIN;
CREATE TABLE cf (id int, a int, s text) USING iheap WITH (autovacuum_enabled = off);
COPY cf FROM PROGRAM 'seq 1 500 | sed "s/.*/&\t&\tcopied &/"' WITH (FREEZE);
COMMIT;
SELECT count(*) FILTER (WHERE 'COMPACT' = ANY (flags)) AS compact, count(*)
FROM iheap_page_items('cf', 1);
SELECT count(*), sum(a) FROM cf;
SELECT * FROM iheap_check('cf');

-- "auto" compacts as many rows per page as the space fillfactor keeps free
-- lets grow back (4 bytes each)
SET iheap.compact_headers = auto;
CREATE TABLE ca (id int, a int, s text)
  USING iheap WITH (fillfactor = 90, autovacuum_enabled = off);
INSERT INTO ca SELECT g, g, 'row ' || g FROM generate_series(1, 1000) g;
VACUUM FULL ca;
SELECT count(*) FILTER (WHERE 'COMPACT' = ANY (flags)) AS compact, count(*)
FROM iheap_page_items('ca', 1);
ALTER TABLE ca SET (fillfactor = 98);
VACUUM FULL ca;
SELECT count(*) FILTER (WHERE 'COMPACT' = ANY (flags)) AS compact, count(*)
FROM iheap_page_items('ca', 1);
SELECT count(*), sum(a) FROM ca;
SELECT * FROM iheap_check('ca');

-- materialized views
SET iheap.compact_headers = always;
CREATE MATERIALIZED VIEW cm USING iheap AS SELECT id, a, s FROM ct WHERE id <= 300;
SELECT count(*) FILTER (WHERE 'COMPACT' = ANY (flags)) > 0 AS compact
FROM iheap_page_items('cm', 1);
REFRESH MATERIALIZED VIEW cm;
SELECT count(*) FILTER (WHERE 'COMPACT' = ANY (flags)) > 0 AS compact
FROM iheap_page_items('cm', 1);
SELECT count(*), sum(a) FROM cm;
SELECT * FROM iheap_check('cm');

-- "off": none
SET iheap.compact_headers = off;
VACUUM FULL ct;
SELECT count(*) FILTER (WHERE 'COMPACT' = ANY (flags)) AS compact
FROM iheap_page_items('ct', 1);
RESET iheap.compact_headers;

-- rows with aligned values or a body too short for a deletion's header in
-- its storage stay full
SET iheap.compact_headers = always;
CREATE TABLE cx (a int2, t text) USING iheap WITH (autovacuum_enabled = off);
INSERT INTO cx VALUES (1, NULL), (2, repeat('x', 300)), (3, 'short');
VACUUM FULL cx;
SELECT lp, lp_len, hoff, flags FROM iheap_page_items('cx', 1) ORDER BY lp;
SELECT * FROM iheap_check('cx');
RESET iheap.compact_headers;

DROP MATERIALIZED VIEW cm;
DROP TABLE ct, cf, ca, cx;
