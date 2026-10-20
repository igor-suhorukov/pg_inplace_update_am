--
-- Verification of healthy relations: iheap_check() finds nothing, and
-- dmbtree_check() and dmbtree_parent_check() return.  (t/004_check.pl
-- corrupts pages and sees them report.)  amcheck checks the btree indexes
-- of the same tables, through the table AM's index build scan.
--
CREATE EXTENSION amcheck;

-- every dmbtree index of a table, with every option; btree ones with amcheck
CREATE FUNCTION check_indexes(rel regclass, parent boolean DEFAULT true)
RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE
  idx regclass;
  am name;
  uniq boolean;
BEGIN
  FOR idx, am, uniq IN
    SELECT i.indexrelid::regclass, a.amname, i.indisunique
    FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
         JOIN pg_am a ON a.oid = c.relam
    WHERE i.indrelid = rel ORDER BY i.indexrelid::regclass::text
  LOOP
    IF am = 'dmbtree' THEN
      PERFORM dmbtree_check(idx, true, uniq, true);
      IF parent THEN
        PERFORM dmbtree_parent_check(idx, true, true, uniq, true);
      END IF;
    ELSIF am = 'btree' THEN
      PERFORM bt_index_check(idx, true, uniq);
      IF parent THEN
        PERFORM bt_index_parent_check(idx, true, true, uniq);
      END IF;
    END IF;
    RETURN NEXT idx::text || ' ' || am;
  END LOOP;
END $$;

CREATE TABLE ck (id int PRIMARY KEY, k int, v int8, t timestamptz, s text,
                 n numeric, big text)
  USING iheap WITH (autovacuum_enabled = off);
CREATE INDEX ck_k ON ck (k);
CREATE INDEX ck_v ON ck USING btree (v);
CREATE INDEX ck_s ON ck (s);
CREATE INDEX ck_kv ON ck (k) INCLUDE (v);
CREATE UNIQUE INDEX ck_uk ON ck USING dmbtree (k, id);
INSERT INTO ck
SELECT g, g, g * 1000, '2026-01-01'::timestamptz + g * interval '1 hour',
       'row ' || g, g / 7.0,
       CASE WHEN g % 100 = 0 THEN repeat(md5(g::text), 300) END
FROM generate_series(1, 2000) g;

SELECT * FROM iheap_check('ck');
SELECT check_indexes('ck');

-- a history: in place (also of dmbtree keys and of TOASTed values), moves,
-- deletions, row locks, savepoints, eager and lazy rollback
UPDATE ck SET n = n + 1 WHERE id % 3 = 0;
UPDATE ck SET k = k + 10000, s = s || '+' WHERE id % 5 = 0;
UPDATE ck SET v = -v WHERE id % 7 = 0;
UPDATE ck SET big = repeat(md5((id + 1)::text), 300) WHERE id % 200 = 0;
UPDATE ck SET big = NULL WHERE id % 300 = 0;
DELETE FROM ck WHERE id % 11 = 0;
SELECT count(*) FROM (SELECT 1 FROM ck WHERE id % 13 = 0 FOR KEY SHARE) x;
SELECT count(*) FROM (SELECT 1 FROM ck WHERE id % 17 = 0 FOR SHARE) x;
BEGIN;
UPDATE ck SET k = k + 1 WHERE id % 19 = 0;
SAVEPOINT a;
UPDATE ck SET s = 'gone' WHERE id % 23 = 0;
DELETE FROM ck WHERE id % 29 = 0;
ROLLBACK TO a;
UPDATE ck SET t = t + interval '1 day' WHERE id % 31 = 0;
COMMIT;
BEGIN;
UPDATE ck SET k = -k WHERE id < 100;
ROLLBACK;
SET iheap.foreground_undo_limit = 0;
BEGIN;
UPDATE ck SET s = 'lazy', k = k + 3 WHERE id % 37 = 0;
UPDATE ck SET v = v + 1 WHERE id % 41 = 0;
ROLLBACK;
RESET iheap.foreground_undo_limit;

SELECT * FROM iheap_check('ck');
SELECT check_indexes('ck');

-- inside a transaction with changes of its own
BEGIN;
UPDATE ck SET k = k + 7 WHERE id % 43 = 0;
DELETE FROM ck WHERE id % 47 = 0;
INSERT INTO ck (id, k, s) VALUES (5001, 5001, 'new');
SELECT * FROM iheap_check('ck');
SELECT check_indexes('ck', false);
ROLLBACK;

-- columns added and dropped
ALTER TABLE ck ADD COLUMN extra int DEFAULT 5;
ALTER TABLE ck DROP COLUMN t;
UPDATE ck SET extra = id WHERE id % 53 = 0;
SELECT * FROM iheap_check('ck');

-- many undo pages, extents beyond the first, then VACUUM (stubs, unused
-- line pointers, visibility map bits) and discard
UPDATE ck SET n = n + 1;
UPDATE ck SET n = n + 1;
UPDATE ck SET k = k + 1 WHERE id % 2 = 0;
SELECT nextents > 1 AS several_extents FROM iheap_metapage_info('ck');
SELECT * FROM iheap_check('ck');
VACUUM ck;
SELECT * FROM iheap_check('ck');
SELECT check_indexes('ck');
SELECT iheap_discard('ck') >= 0 AS discarded;
VACUUM ck;
SELECT * FROM iheap_check('ck');
SELECT check_indexes('ck');

-- VACUUM FULL and CLUSTER rewrite the table and its undo
VACUUM FULL ck;
SELECT * FROM iheap_check('ck');
SELECT check_indexes('ck');
CLUSTER ck USING ck_pkey;
SELECT * FROM iheap_check('ck');
SELECT check_indexes('ck');

-- without the index cross-check (t/004_check.pl does it in ranges of blocks)
SELECT * FROM iheap_check('ck', check_indexes => false);

-- other kinds of relations
CREATE TEMP TABLE ckt (id int PRIMARY KEY, s text) USING iheap;
INSERT INTO ckt SELECT g, 't' || g FROM generate_series(1, 300) g;
UPDATE ckt SET s = s || '!' WHERE id % 2 = 0;
DELETE FROM ckt WHERE id % 3 = 0;
SELECT * FROM iheap_check('ckt');
CREATE UNLOGGED TABLE cku (id int, k int) USING iheap;
CREATE INDEX cku_k ON cku (k);
INSERT INTO cku SELECT g, g FROM generate_series(1, 300) g;
UPDATE cku SET k = -k WHERE id % 2 = 0;
SELECT * FROM iheap_check('cku');
SELECT check_indexes('cku');
CREATE TABLE ckp (id int, k int) PARTITION BY RANGE (id);
CREATE TABLE ckp1 PARTITION OF ckp FOR VALUES FROM (0) TO (100) USING iheap;
CREATE TABLE ckp2 PARTITION OF ckp FOR VALUES FROM (100) TO (200) USING iheap;
CREATE INDEX ckp_k ON ckp USING dmbtree (k);
INSERT INTO ckp SELECT g, g FROM generate_series(0, 199) g;
UPDATE ckp SET id = id + 100, k = k + 1 WHERE id < 50;
SELECT * FROM iheap_check('ckp1');
SELECT * FROM iheap_check('ckp2');
SELECT check_indexes('ckp1'), check_indexes('ckp2');
CREATE MATERIALIZED VIEW ckm USING iheap AS SELECT id, k FROM ck WHERE id < 500;
SELECT * FROM iheap_check('ckm');
REFRESH MATERIALIZED VIEW ckm;
SELECT * FROM iheap_check('ckm');

-- what cannot be checked
SELECT * FROM iheap_check('ck_k');
CREATE TABLE ckh (id int) USING heap;
SELECT * FROM iheap_check('ckh');
SELECT * FROM iheap_check('ckp');
SELECT dmbtree_check('ck_v');
SELECT dmbtree_check('ck');

DROP MATERIALIZED VIEW ckm;
DROP TABLE ck, ckt, cku, ckp, ckh;
DROP FUNCTION check_indexes(regclass, boolean);
DROP EXTENSION amcheck;
