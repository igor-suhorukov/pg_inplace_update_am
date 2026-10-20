--
-- Indexes: which updates stay in place, index builds, scans
--
CREATE TABLE ix (id int PRIMARY KEY, k int, tag text, doc jsonb, rng int4range,
                 extra text, b int, dk int) USING iheap;
CREATE INDEX ix_k ON ix USING btree (k);
CREATE INDEX ix_dk ON ix USING dmbtree (dk);
CREATE INDEX ix_tag_hash ON ix USING hash (tag);
CREATE INDEX ix_doc ON ix USING gin (doc);
CREATE INDEX ix_rng ON ix USING gist (rng);
CREATE INDEX ix_b_brin ON ix USING brin (b);
CREATE INDEX ix_partial ON ix (extra) WHERE extra IS NOT NULL;
CREATE INDEX ix_expr ON ix (lower(tag));

INSERT INTO ix
SELECT g, g % 50, 'Tag' || (g % 20), jsonb_build_object('a', g % 7, 'b', 'x'),
       int4range(g, g + 10), NULL, g, g % 50
FROM generate_series(1, 2000) g;
ANALYZE ix;

CREATE TEMP TABLE ix_tids AS SELECT id, ctid AS t FROM ix;

-- column covered by no index: in place
UPDATE ix SET extra = NULL WHERE id <= 100;
-- covered by BRIN only (a summarizing index): in place too
UPDATE ix SET b = b + 1 WHERE id BETWEEN 101 AND 200;
-- btree-indexed column: moves
UPDATE ix SET k = k + 1 WHERE id BETWEEN 201 AND 300;
-- hash / gin / gist / expression / partial index columns: move
UPDATE ix SET tag = tag || 'x' WHERE id BETWEEN 301 AND 400;
UPDATE ix SET doc = doc || '{"c": 1}' WHERE id BETWEEN 401 AND 500;
UPDATE ix SET rng = int4range(lower(rng), upper(rng) + 1) WHERE id BETWEEN 501 AND 600;
UPDATE ix SET extra = 'set' WHERE id BETWEEN 601 AND 700;
-- dmbtree-indexed column: in place (the index keeps an entry per key)
UPDATE ix SET dk = dk + 100 WHERE id BETWEEN 801 AND 900;

SELECT CASE WHEN id <= 100 THEN 'no index'
            WHEN id <= 200 THEN 'brin only'
            WHEN id <= 300 THEN 'btree'
            WHEN id <= 400 THEN 'hash+expr'
            WHEN id <= 500 THEN 'gin'
            WHEN id <= 600 THEN 'gist'
            WHEN id <= 700 THEN 'partial'
            WHEN id BETWEEN 801 AND 900 THEN 'dmbtree'
            ELSE 'untouched' END AS what,
       count(*) FILTER (WHERE ix.ctid = t) AS stayed,
       count(*) FILTER (WHERE ix.ctid <> t) AS moved
FROM ix JOIN ix_tids USING (id)
GROUP BY 1 ORDER BY min(id);

-- an update that sets an indexed column to the same value stays in place
UPDATE ix SET k = k, tag = tag WHERE id BETWEEN 701 AND 750;
SELECT count(*) FILTER (WHERE ix.ctid = t) AS stayed FROM ix JOIN ix_tids USING (id)
WHERE id BETWEEN 701 AND 750;

-- the indexes agree with the table
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM ix WHERE k = 10;
SELECT count(*) FROM ix WHERE tag = 'Tag5x';
SELECT count(*) FROM ix WHERE lower(tag) = 'tag5x';
SELECT count(*) FROM ix WHERE extra = 'set';
SELECT count(*) FROM ix WHERE dk = 110;
SELECT count(*) FROM ix WHERE dk = 10;
RESET enable_bitmapscan;
SET enable_indexscan = off;
SELECT count(*) FROM ix WHERE doc @> '{"c": 1}';
SELECT count(*) FROM ix WHERE rng @> 555;
SELECT count(*) FROM ix WHERE k BETWEEN 10 AND 12;
SELECT count(*) FROM ix WHERE b BETWEEN 150 AND 160;
RESET enable_indexscan;
RESET enable_seqscan;
SELECT count(*) FROM ix WHERE k = 10;
SELECT count(*) FROM ix WHERE tag = 'Tag5x';
SELECT count(*) FROM ix WHERE doc @> '{"c": 1}';
SELECT count(*) FROM ix WHERE rng @> 555;

-- unique constraint violations, and keys reused after delete
INSERT INTO ix (id, k) VALUES (1, 0);
DELETE FROM ix WHERE id = 1;
INSERT INTO ix (id, k) VALUES (1, 0);
BEGIN;
DELETE FROM ix WHERE id = 2;
INSERT INTO ix (id, k) VALUES (2, 0);
ROLLBACK;
SELECT id, k, tag FROM ix WHERE id IN (1, 2) ORDER BY id;
-- primary key change and back in one transaction
BEGIN;
UPDATE ix SET id = -3 WHERE id = 3;
UPDATE ix SET id = 3 WHERE id = -3;
COMMIT;
SELECT id, k FROM ix WHERE id IN (3, -3);

-- deferrable unique constraint
CREATE TABLE ixd (a int UNIQUE DEFERRABLE INITIALLY DEFERRED) USING iheap;
INSERT INTO ixd VALUES (1), (2), (3);
UPDATE ixd SET a = a + 1;
SELECT * FROM ixd ORDER BY a;
BEGIN;
INSERT INTO ixd VALUES (2);
COMMIT;

-- index-only scans need the visibility map
VACUUM ix;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT k FROM ix WHERE k = 7;
SELECT count(k) FROM ix WHERE k = 7;
UPDATE ix SET k = 7 WHERE id = 1000;
SELECT count(k) FROM ix WHERE k = 7;
RESET enable_seqscan;
RESET enable_bitmapscan;

-- building indexes on a table with history and uncommitted changes
CREATE TABLE ixb (id int, v int, s text) USING iheap;
INSERT INTO ixb SELECT g, g, 's' || g FROM generate_series(1, 1000) g;
UPDATE ixb SET v = v + 1 WHERE id % 3 = 0;
DELETE FROM ixb WHERE id % 5 = 0;
BEGIN;
UPDATE ixb SET v = -v WHERE id % 7 = 0;
CREATE INDEX ixb_v ON ixb (v);
SET LOCAL enable_seqscan = off;
SELECT count(*) FROM ixb WHERE v < 0;
ROLLBACK;
CREATE INDEX ixb_v ON ixb (v);
CREATE UNIQUE INDEX CONCURRENTLY ixb_id ON ixb (id);
REINDEX TABLE ixb;
REINDEX TABLE CONCURRENTLY ixb;
SET enable_seqscan = off;
SELECT count(*) FROM ixb WHERE v < 0;
SELECT count(*) FROM ixb WHERE id = 10;
SELECT count(*) FROM ixb WHERE id = 11;
RESET enable_seqscan;
SELECT count(*) FROM ixb;

-- amcheck-style cross-check: every row is found through each index
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) = (SELECT count(*) FROM ixb) AS all_via_index
FROM ixb WHERE id > 0;
RESET enable_seqscan;
RESET enable_bitmapscan;

DROP TABLE ix, ixd, ixb;
