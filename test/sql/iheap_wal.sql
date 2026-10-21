--
-- Deleted and moved rows keep their bodies on the page until every snapshot
-- sees the change (DESIGN.md §21 D40); the size of iheap's WAL records
-- (§21 D42); table rewrites write through the bulk writer (§21 D41).
--
CREATE EXTENSION IF NOT EXISTS pg_walinspect;

-- the undo records of a relation: every undo page's
CREATE FUNCTION undo_records(rel regclass)
RETURNS TABLE (type text, len int, xid xid, target_block int8, target_offset int4)
LANGUAGE plpgsql AS $$
DECLARE
  b int8;
BEGIN
  FOR b IN 1 .. pg_relation_size(rel) / current_setting('block_size')::int - 1 LOOP
    BEGIN
      RETURN QUERY SELECT r.type, r.len, r.xid, r.target_block, r.target_offset
                   FROM iheap_undo_page_records(rel, b) r;
    EXCEPTION WHEN invalid_parameter_value THEN
      NULL;					-- not an undo page
    END;
  END LOOP;
END $$;

-- WAL records of a resource manager since an LSN, by type: the smallest and
-- largest without a full-page image (the first change of a page after a
-- checkpoint carries one, so only those are counted)
CREATE FUNCTION wal_sizes(since pg_lsn, rmgr text)
RETURNS TABLE (record_type text, min_len int)
LANGUAGE sql AS $$
  SELECT record_type, min(record_length)
  FROM pg_get_wal_records_info(since, pg_current_wal_flush_lsn())
  WHERE resource_manager = rmgr AND fpi_length = 0
  GROUP BY record_type ORDER BY record_type
$$;

-- WAL records that refer to a relation's main fork since an LSN, by type
CREATE FUNCTION wal_records_of(since pg_lsn, rel regclass)
RETURNS TABLE (resource_manager text, record_type text, n bigint)
LANGUAGE sql AS $$
  SELECT resource_manager, record_type, count(*)
  FROM pg_get_wal_records_info(since, pg_current_wal_flush_lsn())
  WHERE block_ref LIKE '%/' || pg_relation_filenode(rel) || ' fork main %'
  GROUP BY 1, 2 ORDER BY 1, 2
$$;

-- ------------------------------------------------------------------------
-- A. A move and a deletion write the old row's header to undo, not its
--    body; the body stays on the page while some snapshot may need it.
-- ------------------------------------------------------------------------
CREATE TABLE mv (id int PRIMARY KEY, k int, v int, pad text)
  USING iheap WITH (autovacuum_enabled = off);
-- a btree index: an update of k moves the row (rule b)
CREATE INDEX mv_k ON mv USING btree (k);
INSERT INTO mv SELECT g, g, 0, repeat('p', 100) FROM generate_series(1, 40) g;
VACUUM mv;

-- a prepared transaction holds back the horizon
CREATE TABLE holder (x int);
BEGIN;
INSERT INTO holder VALUES (1);
PREPARE TRANSACTION 'hold_mv';

UPDATE mv SET k = k + 1000 WHERE id <= 10;		-- moves
DELETE FROM mv WHERE id BETWEEN 11 AND 20;		-- deletions
UPDATE mv SET v = v + 1 WHERE id BETWEEN 21 AND 30;	-- in place

-- MOVE records carry the new TID (6 bytes) and DELETE records nothing;
-- an in-place update's record carries the changed bytes
SELECT type, count(*), max(len) < 50 AS short
FROM undo_records('mv') WHERE type IN ('MOVE', 'DELETE', 'UPDATE')
GROUP BY type ORDER BY type;

-- pruning and VACUUM keep the bodies: the prepared transaction's snapshot
-- horizon is older than the changes
VACUUM mv;
SELECT live_items, deleted_items, stub_items, dead_items
FROM iheap_relation_stats('mv');
SELECT count(*), sum(k), sum(v) FROM mv;
SELECT * FROM iheap_check('mv');

-- once nobody can see the old rows, VACUUM frees their space
COMMIT PREPARED 'hold_mv';
VACUUM mv;
SELECT live_items, deleted_items, stub_items, dead_items
FROM iheap_relation_stats('mv');
SELECT count(*), sum(k), sum(v) FROM mv;
SELECT * FROM iheap_check('mv');

-- a deletion and a move that roll back restore the header only
BEGIN;
DELETE FROM mv WHERE id = 21;
UPDATE mv SET k = -k WHERE id = 22;
ROLLBACK;
SELECT id, k, v FROM mv WHERE id IN (21, 22) ORDER BY id;
SELECT * FROM iheap_check('mv');

-- ------------------------------------------------------------------------
-- B. WAL record sizes.  Tuple headers travel without their xid and with
--    their undo pointer as an offset on the record's undo page; an in-place
--    update's undo delta takes its prefix and suffix from the update record;
--    undo records go in their page encoding less what redo derives.
-- ------------------------------------------------------------------------
CREATE TABLE wi (id int PRIMARY KEY, k int, v int, pad text)
  USING iheap WITH (autovacuum_enabled = off);
CREATE INDEX wi_k ON wi USING btree (k);
CREATE TABLE wh (id int PRIMARY KEY, k int, v int, pad text)
  WITH (autovacuum_enabled = off);
CREATE INDEX wh_k ON wh USING btree (k);
INSERT INTO wi SELECT g, g, 0, repeat('w', 40) FROM generate_series(1, 30) g;
INSERT INTO wh SELECT g, g, 0, repeat('w', 40) FROM generate_series(1, 30) g;
-- every page changed once already, so that the records below carry no
-- full-page image unless a checkpoint comes in between
UPDATE wi SET v = 1;
UPDATE wh SET v = 1;
SELECT pg_current_wal_insert_lsn() AS lsn0 \gset

-- each statement on 5 rows, each row in a transaction of its own
CREATE PROCEDURE wal_round(t regclass) LANGUAGE plpgsql AS $$
BEGIN
  FOR i IN 1 .. 5 LOOP
    EXECUTE format('INSERT INTO %s VALUES ($1, $1, 0, repeat(''w'', 40))', t) USING 100 + i;
    COMMIT;
    EXECUTE format('UPDATE %s SET v = v + 1 WHERE id = $1', t) USING i;
    COMMIT;
    EXECUTE format('UPDATE %s SET k = k + 1000 WHERE id = $1', t) USING 5 + i;
    COMMIT;
    EXECUTE format('SELECT 1 FROM %s WHERE id = $1 FOR UPDATE', t) USING 10 + i;
    COMMIT;
    EXECUTE format('DELETE FROM %s WHERE id = $1', t) USING 15 + i;
    COMMIT;
  END LOOP;
END $$;
CALL wal_round('wi');
SELECT pg_current_wal_insert_lsn() AS lsn1 \gset
CALL wal_round('wh');

SELECT * FROM wal_sizes(:'lsn0', 'iheap')
WHERE record_type IN ('INSERT', 'UPDATE', 'MOVE', 'LOCK', 'DELETE');
SELECT * FROM wal_sizes(:'lsn1', 'Heap')
WHERE record_type IN ('INSERT', 'HOT_UPDATE', 'UPDATE', 'LOCK', 'DELETE');
SELECT count(*), sum(k), sum(v) FROM wi;
SELECT count(*), sum(k), sum(v) FROM wh;

-- COPY: a record per page, its rows' headers without xid and undo pointer
SELECT pg_current_wal_insert_lsn() AS lsn2 \gset
COPY wi FROM PROGRAM 'seq 1001 1300 | sed "s/.*/&\t&\t0\tccccccccccccccccccccccccccccc/"';
SELECT count(*) FILTER (WHERE record_type = 'INSERT') AS inserts,
       count(*) FILTER (WHERE record_type = 'MULTI_INSERT') > 0 AS multi_inserts
FROM wal_records_of(:'lsn2', 'wi') w, generate_series(1, w.n);
SELECT count(*), sum(k), sum(v) FROM wi;
SELECT * FROM iheap_check('wi');

-- ------------------------------------------------------------------------
-- C. VACUUM FULL and CLUSTER write the new relation through the bulk
--    writer: whole pages in WAL, no iheap records.
-- ------------------------------------------------------------------------
SELECT pg_current_wal_insert_lsn() AS lsn3 \gset
VACUUM FULL wi;
SELECT resource_manager, record_type FROM wal_records_of(:'lsn3', 'wi');
SELECT count(*), sum(k), sum(v) FROM wi;
SELECT * FROM iheap_check('wi');
CLUSTER wi USING wi_pkey;
SELECT count(*), sum(k), sum(v) FROM wi;
SELECT (array_agg(id ORDER BY ctid))[1:5] AS first_ids FROM wi;
SELECT * FROM iheap_check('wi');

-- a rewrite keeps the versions older snapshots need, in undo of its own
BEGIN;
INSERT INTO holder VALUES (2);
PREPARE TRANSACTION 'hold_rw';
UPDATE wi SET v = v + 10 WHERE id <= 50;
DELETE FROM wi WHERE id BETWEEN 1001 AND 1010;
VACUUM FULL wi;
SELECT live_undo_pages > 0 AS undo_kept FROM iheap_metapage_info('wi');
SELECT type, count(*) FROM undo_records('wi') GROUP BY type ORDER BY type;
SELECT * FROM iheap_check('wi');
COMMIT PREPARED 'hold_rw';
VACUUM wi;
SELECT live_undo_pages FROM iheap_metapage_info('wi');
SELECT count(*), sum(k), sum(v) FROM wi;
SELECT * FROM iheap_check('wi');

-- ------------------------------------------------------------------------
-- D. Moves while a snapshot is held (§21 D45).  A move to another page
--    keeps the old page locked while it locks the new one: no LOCK record.
--    A row that leaves a page whose bodies a snapshot holds takes its body
--    to undo, and the room it leaves comes back when the move commits.
-- ------------------------------------------------------------------------
CREATE TABLE fm (id int PRIMARY KEY, k int, pad text)
  USING iheap WITH (autovacuum_enabled = off);
CREATE INDEX fm_k ON fm USING btree (k);
INSERT INTO fm SELECT g, g, repeat('f', 100) FROM generate_series(1, 100) g;
VACUUM fm;
SELECT (ctid::text::point)[0]::int AS fmblk FROM fm WHERE id = 1 \gset

BEGIN;
INSERT INTO holder VALUES (3);
PREPARE TRANSACTION 'hold_fm';
SELECT pg_current_wal_insert_lsn() AS lsn4 \gset

-- the first move off the full page leaves its body there; at the second,
-- the page waits for the first mover, committed, whom the prepared
-- transaction's snapshot holds: its body goes to undo, its item is a stub
UPDATE fm SET k = -k WHERE id = 1;
UPDATE fm SET k = -k WHERE id = 2;
-- that move committed: its room is back, and the next row moves on the page
UPDATE fm SET k = -k WHERE id = 3;
SELECT id, (ctid::text::point)[0] = :fmblk AS same_page
FROM fm WHERE id <= 3 ORDER BY id;
SELECT target_offset, len > 50 AS with_body
FROM undo_records('fm') WHERE type = 'MOVE' ORDER BY target_offset;
SELECT live_items, deleted_items, stub_items FROM iheap_relation_stats('fm');
SELECT record_type, n FROM wal_records_of(:'lsn4', 'fm')
WHERE resource_manager = 'iheap' AND record_type IN ('LOCK', 'MOVE', 'PRUNE');

-- a move off the page that rolls back gets its body back from undo
BEGIN;
UPDATE fm SET k = -k WHERE id = 4;
SELECT (ctid::text::point)[0] = :fmblk AS same_page FROM fm WHERE id = 4;
ROLLBACK;
SELECT id, k, (ctid::text::point)[0] = :fmblk AS same_page FROM fm WHERE id = 4;
SELECT live_items, deleted_items, stub_items FROM iheap_relation_stats('fm');
SELECT * FROM iheap_check('fm');

-- without a snapshot holding them, the bodies a statement leaves on a full
-- page are its own: rows it moves off the page keep theirs there
COMMIT PREPARED 'hold_fm';
VACUUM fm;
BEGIN;
UPDATE fm SET k = k + 1000 WHERE id BETWEEN 5 AND 30;
SELECT pg_current_xact_id()::xid AS bulkxid \gset
COMMIT;
SELECT count(*) AS moves, count(*) FILTER (WHERE len > 50) AS with_body
FROM undo_records('fm') WHERE type = 'MOVE' AND xid = :'bulkxid';
SELECT count(*), sum(k) FROM fm;
SELECT * FROM iheap_check('fm');

DROP TABLE mv, wi, wh, fm, holder;
DROP PROCEDURE wal_round(regclass);
DROP FUNCTION undo_records(regclass);
DROP FUNCTION wal_sizes(pg_lsn, text);
DROP FUNCTION wal_records_of(pg_lsn, regclass);
