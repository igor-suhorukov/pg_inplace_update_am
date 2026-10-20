--
-- Large values: compression and out-of-line storage in a heap TOAST table
--
SET default_toast_compression = pglz;
CREATE TABLE it (id int PRIMARY KEY, small text, big text, b bytea) USING iheap;
SELECT reltoastrelid <> 0 AS has_toast FROM pg_class WHERE relname = 'it';

-- incompressible data so that values really go out of line
CREATE FUNCTION rnd_text(n int, seed float8) RETURNS text LANGUAGE sql AS $$
  SELECT string_agg(md5(seed::text || g::text), '') FROM generate_series(1, n / 32 + 1) g
$$;

INSERT INTO it VALUES
  (1, 'a', repeat('x', 100000), NULL),                     -- compressible
  (2, 'b', rnd_text(20000, 0.2), NULL),                    -- external
  (3, 'c', rnd_text(5000, 0.3), decode(rnd_text(3000, 0.4), 'escape')),
  (4, 'd', 'short', NULL);
SELECT id, length(big), md5(big) = md5(CASE id WHEN 1 THEN repeat('x', 100000)
                                         WHEN 2 THEN rnd_text(20000, 0.2)
                                         WHEN 3 THEN rnd_text(5000, 0.3)
                                         ELSE 'short' END) AS ok,
       pg_column_compression(big) AS compression
FROM it ORDER BY id;
-- every stored row is below the TOAST target: values compressed or external
SELECT lp, lp_len <= 2048 AS below_target FROM iheap_page_items('it', 1) ORDER BY lp;

-- update a small column: the toast pointer is kept (in place)
CREATE TEMP TABLE it_tids AS SELECT id, ctid AS t FROM it;
UPDATE it SET small = small || '!' WHERE id IN (1, 2, 3);
SELECT id, small, it.ctid = t AS same_tid, md5(big) = md5(rnd_text(20000, 0.2)) AS big2_ok
FROM it JOIN it_tids USING (id) WHERE id IN (1, 2, 3) ORDER BY id;

-- replace a toasted value; old snapshots of the cursor still see the old one
BEGIN;
DECLARE c CURSOR FOR SELECT id, md5(big) = md5(rnd_text(20000, 0.2)) AS old_value
  FROM it WHERE id = 2;
UPDATE it SET big = rnd_text(30000, 0.5) WHERE id = 2;
FETCH ALL FROM c;
SELECT length(big), md5(big) = md5(rnd_text(30000, 0.5)) AS new_value FROM it WHERE id = 2;
COMMIT;

-- rollback restores the old pointer
BEGIN;
UPDATE it SET big = 'tiny' WHERE id = 2;
DELETE FROM it WHERE id = 3;
ROLLBACK;
SELECT id, length(big), length(b) FROM it ORDER BY id;
SELECT md5(big) = md5(rnd_text(30000, 0.5)) AS still_new FROM it WHERE id = 2;

-- moving a row with toasted values (key change)
UPDATE it SET id = 30 WHERE id = 3;
SELECT id, md5(big) = md5(rnd_text(5000, 0.3)) AS ok,
       b = decode(rnd_text(3000, 0.4), 'escape') AS b_ok FROM it WHERE id = 30;

-- delete frees the toast values (after vacuum of the toast table)
DELETE FROM it WHERE id = 2;
VACUUM it;
SELECT count(*) FROM it;

-- a wide row whose plain values exceed a page cannot be stored
CREATE TABLE it_plain (a text STORAGE PLAIN) USING iheap;
INSERT INTO it_plain VALUES (repeat('z', 9000));
INSERT INTO it_plain VALUES (repeat('z', 7000));
SELECT length(a) FROM it_plain;

-- main storage: compressed in line
CREATE TABLE it_main (a text STORAGE MAIN) USING iheap;
INSERT INTO it_main VALUES (repeat('m', 50000));
SELECT length(a), pg_column_compression(a) IS NOT NULL AS compressed FROM it_main;
UPDATE it_main SET a = a || 'n';
SELECT length(a) FROM it_main;

-- many toasted rows, updated in place repeatedly
CREATE TABLE it_many (id int PRIMARY KEY, n int, v text) USING iheap;
INSERT INTO it_many SELECT g, 0, rnd_text(4000, g) FROM generate_series(1, 50) g;
UPDATE it_many SET n = n + 1;
UPDATE it_many SET n = n + 1;
UPDATE it_many SET v = rnd_text(4000, id + 1000) WHERE id % 2 = 0;
SELECT sum(n), count(*) FILTER (WHERE md5(v) = md5(rnd_text(4000, id + 1000))) AS replaced,
       count(*) FILTER (WHERE md5(v) = md5(rnd_text(4000, id))) AS original
FROM it_many;

DROP TABLE it, it_plain, it_main, it_many;
DROP FUNCTION rnd_text(int, float8);
