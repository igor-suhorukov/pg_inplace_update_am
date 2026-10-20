--
-- Space use under frequent updates, compared with heap.
--
-- Every update below is its own transaction (a procedure that commits after
-- each one).  Sizes are compared as ratios so that the test does not depend
-- on exact page layouts.
--
CREATE PROCEDURE bump(tab regclass, nrows int, nupdates int)
LANGUAGE plpgsql AS $$
BEGIN
  FOR i IN 1 .. nupdates LOOP
    EXECUTE format('UPDATE %s SET counter = counter + 1 WHERE id = $1', tab)
      USING (i % nrows) + 1;
    COMMIT;
  END LOOP;
END $$;

-- ------------------------------------------------------------------------
-- A. Same-size updates of a non-indexed column on full pages.
--    heap: HOT needs room on the page for a second version, so the first
--    update of each row moves it to another page.
--    iheap: the row is overwritten in place; old versions go to undo.
-- ------------------------------------------------------------------------
CREATE TABLE ia (id int PRIMARY KEY, counter int, payload text) USING iheap;
CREATE TABLE ha (id int PRIMARY KEY, counter int, payload text);
INSERT INTO ia SELECT g, 0, repeat('p', 100) FROM generate_series(1, 2000) g;
INSERT INTO ha SELECT g, 0, repeat('p', 100) FROM generate_series(1, 2000) g;
VACUUM ia, ha;
SELECT data_pages AS ia_data0 FROM iheap_relation_stats('ia') \gset
SELECT pg_relation_size('ha') / 8192 AS ha_pages0 \gset
SELECT pg_relation_size('ia_pkey') AS ia_idx0 \gset

CALL bump('ia', 2000, 6000);
CALL bump('ha', 2000, 6000);

SELECT (SELECT sum(counter) FROM ia) AS ia_sum, (SELECT sum(counter) FROM ha) AS ha_sum;

-- iheap: no row moved, no new data pages, no index growth
SELECT data_pages = :ia_data0 AS ia_same_data_pages,
       live_items = 2000 AS ia_no_extra_items,
       pg_relation_size('ia_pkey') = :ia_idx0 AS ia_index_unchanged
FROM iheap_relation_stats('ia');
-- heap: grew
SELECT pg_relation_size('ha') / 8192 > :ha_pages0 AS ha_grew;

-- ------------------------------------------------------------------------
-- B. Updates while an old transaction holds back cleanup.
--    heap: nothing can be pruned, every version stays in the table.
--    iheap: rows stay in place; versions accumulate in undo and the undo is
--    recycled once the old transaction is gone.
-- ------------------------------------------------------------------------
CREATE TABLE ib (id int PRIMARY KEY, counter int, payload text) USING iheap
  WITH (fillfactor = 80);
CREATE TABLE hb (id int PRIMARY KEY, counter int, payload text)
  WITH (fillfactor = 80);
INSERT INTO ib SELECT g, 0, repeat('p', 100) FROM generate_series(1, 1000) g;
INSERT INTO hb SELECT g, 0, repeat('p', 100) FROM generate_series(1, 1000) g;
VACUUM ib, hb;
SELECT data_pages AS ib_data0 FROM iheap_relation_stats('ib') \gset
SELECT pg_relation_size('hb') AS hb_size0 \gset

-- the prepared transaction's xid keeps the cleanup horizon back
CREATE TABLE holder (x int);
BEGIN;
INSERT INTO holder VALUES (1);
PREPARE TRANSACTION 'holdback';

CALL bump('ib', 1000, 5000);
CALL bump('hb', 1000, 5000);

SELECT data_pages = :ib_data0 AS ib_same_data_pages,
       undo_pages > 0 AS ib_has_undo
FROM iheap_relation_stats('ib');
SELECT pg_relation_size('hb') > 3 * :hb_size0 AS hb_bloated;
SELECT pg_relation_size('ib') AS ib_size_held \gset

COMMIT PREPARED 'holdback';
VACUUM ib, hb;

-- iheap: undo discarded, every row frozen; heap: the file stays big
SELECT live_undo_pages = 0 AS ib_undo_discarded FROM iheap_metapage_info('ib');
SELECT live_items, frozen_items FROM iheap_relation_stats('ib');
SELECT pg_relation_size('hb') > 3 * :hb_size0 AS hb_still_big;

-- more updates reuse the recycled undo space instead of growing the file
CALL bump('ib', 1000, 3000);
SELECT pg_relation_size('ib') <= :ib_size_held AS ib_reused_undo;
SELECT sum(counter) FROM ib;

-- one VACUUM discards all the undo the horizon allows, however much: a
-- writer passes at most 1024 undo pages at a time, VACUUM and
-- iheap_discard() go on (§21 D9)
CREATE TABLE iu (id int, pad text) USING iheap WITH (autovacuum_enabled = off);
INSERT INTO iu SELECT g, repeat('u', 200) FROM generate_series(1, 50000) g;
VACUUM iu;
UPDATE iu SET pad = repeat('v', 200);
SELECT live_undo_pages > 1024 AS iu_much_undo FROM iheap_metapage_info('iu');
VACUUM iu;
SELECT live_undo_pages AS iu_undo_left FROM iheap_metapage_info('iu');
UPDATE iu SET pad = repeat('w', 200);
SELECT iheap_discard('iu') > 1024 AS iu_discarded_all;
SELECT live_undo_pages AS iu_undo_left FROM iheap_metapage_info('iu');
DROP TABLE iu;

-- ------------------------------------------------------------------------
-- C. Rows that grow and shrink keep their TIDs while the page has room.
-- ------------------------------------------------------------------------
CREATE TABLE ic (id int PRIMARY KEY, payload text) USING iheap;
INSERT INTO ic SELECT g, repeat('a', 50) FROM generate_series(1, 100) g;
CREATE TEMP TABLE ic_tids AS SELECT id, ctid AS t FROM ic;
CREATE PROCEDURE resize(nupdates int) LANGUAGE plpgsql AS $$
BEGIN
  FOR g IN 1 .. nupdates LOOP
    UPDATE ic SET payload = repeat(chr(97 + g % 26), 30 + (g * 7) % 41)
    WHERE id = (g % 100) + 1;
    COMMIT;
  END LOOP;
END $$;
CALL resize(2000);
SELECT count(*) AS moved FROM ic JOIN ic_tids USING (id) WHERE ic.ctid <> t;
SELECT count(*), min(length(payload)) >= 30, max(length(payload)) <= 70 FROM ic;
SELECT data_pages FROM iheap_relation_stats('ic');

-- ------------------------------------------------------------------------
-- D. Delete everything, vacuum, insert again: the space is reused.
--    (The DELETE itself needs undo for the deleted rows; VACUUM recycles it.)
-- ------------------------------------------------------------------------
DELETE FROM ia;
VACUUM ia;
SELECT live_items, deleted_items, stub_items, dead_items FROM iheap_relation_stats('ia');
SELECT pg_relation_size('ia') AS ia_size \gset
INSERT INTO ia SELECT g, 0, repeat('q', 100) FROM generate_series(1, 2000) g;
SELECT pg_relation_size('ia') <= :ia_size AS ia_reused;

DROP TABLE ia, ha, ib, hb, ic, holder;
DROP PROCEDURE bump(regclass, int, int);
DROP PROCEDURE resize(int);
