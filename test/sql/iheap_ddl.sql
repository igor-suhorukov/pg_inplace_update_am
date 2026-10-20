--
-- DDL on iheap tables: access method changes, rewrites, persistence,
-- partitioning, inheritance, truncation
--

-- converting an existing heap table and back
CREATE TABLE conv (id int PRIMARY KEY, v text);
INSERT INTO conv SELECT g, 'v' || g FROM generate_series(1, 500) g;
DELETE FROM conv WHERE id % 10 = 0;
ALTER TABLE conv SET ACCESS METHOD iheap;
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE relname = 'conv';
SELECT count(*), sum(id) FROM conv;
UPDATE conv SET v = v || '!' WHERE id <= 5;
SELECT * FROM conv WHERE id <= 5 ORDER BY id;
ALTER TABLE conv SET ACCESS METHOD heap;
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE relname = 'conv';
SELECT count(*), sum(id), count(*) FILTER (WHERE v LIKE '%!') FROM conv;
DROP TABLE conv;

-- default_table_access_method
SET default_table_access_method = iheap;
CREATE TABLE dflt (a int);
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE relname = 'dflt';
CREATE TABLE dflt_as AS SELECT g AS a FROM generate_series(1, 10) g;
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE relname = 'dflt_as';
SELECT count(*) FROM dflt_as;
RESET default_table_access_method;
DROP TABLE dflt, dflt_as;

-- rewrites: column type change, volatile default, VACUUM FULL, CLUSTER
CREATE TABLE rw (id int PRIMARY KEY, n int, t text) USING iheap;
INSERT INTO rw SELECT g, g, repeat('t', g % 50) FROM generate_series(1, 1000) g;
UPDATE rw SET n = n * 2 WHERE id % 3 = 0;
DELETE FROM rw WHERE id % 7 = 0;
ALTER TABLE rw ALTER COLUMN n TYPE bigint;
ALTER TABLE rw ADD COLUMN r float8 DEFAULT random();
ALTER TABLE rw ADD COLUMN c int DEFAULT 42;
SELECT count(*), sum(n), sum(c), count(r) FROM rw;
VACUUM FULL rw;
CLUSTER rw USING rw_pkey;
SELECT count(*), sum(n), sum(c) FROM rw;
SET enable_seqscan = off;
SELECT count(*) FROM rw WHERE id BETWEEN 100 AND 200;
RESET enable_seqscan;
DROP TABLE rw;

-- persistence changes
CREATE UNLOGGED TABLE pers (id int PRIMARY KEY, v int) USING iheap;
INSERT INTO pers SELECT g, g FROM generate_series(1, 300) g;
UPDATE pers SET v = v + 1;
ALTER TABLE pers SET LOGGED;
SELECT relpersistence FROM pg_class WHERE relname = 'pers';
UPDATE pers SET v = v + 1;
ALTER TABLE pers SET UNLOGGED;
SELECT relpersistence, (SELECT sum(v) FROM pers) FROM pg_class WHERE relname = 'pers';
DROP TABLE pers;

-- partitions, with mixed access methods
CREATE TABLE parted (id int, region text, v int) PARTITION BY LIST (region);
CREATE TABLE parted_e PARTITION OF parted FOR VALUES IN ('east') USING iheap;
CREATE TABLE parted_w PARTITION OF parted FOR VALUES IN ('west') USING heap;
INSERT INTO parted SELECT g, CASE WHEN g % 2 = 0 THEN 'east' ELSE 'west' END, g
FROM generate_series(1, 100) g;
UPDATE parted SET v = v + 1 WHERE region = 'east';
-- moving rows between partitions (delete + insert across access methods)
UPDATE parted SET region = 'west' WHERE id <= 10 AND region = 'east';
UPDATE parted SET region = 'east' WHERE id <= 5 AND region = 'west';
SELECT tableoid::regclass, count(*), sum(v) FROM parted GROUP BY 1 ORDER BY 1;
DROP TABLE parted;

-- inheritance
CREATE TABLE inh_p (id int, v int) USING iheap;
CREATE TABLE inh_c (extra text) INHERITS (inh_p) USING iheap;
INSERT INTO inh_p VALUES (1, 1), (2, 2);
INSERT INTO inh_c VALUES (3, 3, 'c');
UPDATE inh_p SET v = v * 10;
SELECT tableoid::regclass, id, v FROM inh_p ORDER BY id;
DELETE FROM inh_p WHERE id = 3;
SELECT count(*) FROM inh_c;
DROP TABLE inh_p CASCADE;

-- TRUNCATE: transactional, inside savepoints, and in the creating transaction
CREATE TABLE tr (id int PRIMARY KEY, v int) USING iheap;
INSERT INTO tr SELECT g, g FROM generate_series(1, 100) g;
BEGIN;
UPDATE tr SET v = 0 WHERE id <= 10;
SAVEPOINT s;
TRUNCATE tr;
INSERT INTO tr VALUES (1, -1);
ROLLBACK TO s;
SELECT count(*), sum(v) FROM tr;
COMMIT;
SELECT count(*), sum(v) FROM tr;
BEGIN;
CREATE TABLE tr2 (id int) USING iheap;
INSERT INTO tr2 SELECT generate_series(1, 50);
SAVEPOINT s;
INSERT INTO tr2 VALUES (51);
ROLLBACK TO s;
TRUNCATE tr2;
INSERT INTO tr2 VALUES (1);
COMMIT;
SELECT * FROM tr2;
DROP TABLE tr, tr2;

-- temporary tables, ON COMMIT actions
CREATE TEMP TABLE tmp_del (id int, v int) USING iheap ON COMMIT DELETE ROWS;
BEGIN;
INSERT INTO tmp_del SELECT g, g FROM generate_series(1, 20) g;
UPDATE tmp_del SET v = v + 1;
SELECT count(*), sum(v) FROM tmp_del;
COMMIT;
SELECT count(*) FROM tmp_del;
INSERT INTO tmp_del VALUES (1, 1);
SELECT count(*) FROM tmp_del;
CREATE TEMP TABLE tmp_keep (id int PRIMARY KEY, v int) USING iheap;
INSERT INTO tmp_keep SELECT g, g FROM generate_series(1, 50) g;
BEGIN;
UPDATE tmp_keep SET v = -v;
DELETE FROM tmp_keep WHERE id > 40;
ROLLBACK;
SELECT count(*), sum(v) FROM tmp_keep;
UPDATE tmp_keep SET v = v + 1 WHERE id <= 5;
SELECT count(*), sum(v) FROM tmp_keep;
VACUUM tmp_keep;
SELECT count(*), sum(v) FROM tmp_keep;

-- materialized views
CREATE TABLE mvsrc (g int, v int) USING iheap;
INSERT INTO mvsrc SELECT g % 5, g FROM generate_series(1, 100) g;
CREATE MATERIALIZED VIEW mv USING iheap AS SELECT g, sum(v) AS s FROM mvsrc GROUP BY g;
CREATE UNIQUE INDEX mv_g ON mv (g);
SELECT * FROM mv ORDER BY g;
UPDATE mvsrc SET v = v + 1;
REFRESH MATERIALIZED VIEW mv;
REFRESH MATERIALIZED VIEW CONCURRENTLY mv;
SELECT * FROM mv ORDER BY g;
DROP MATERIALIZED VIEW mv;
DROP TABLE mvsrc;

-- COPY in and out, including FREEZE (rows stored frozen, no undo)
CREATE TABLE cp (id int, t text) USING iheap;
COPY cp FROM PROGRAM 'seq 1 1000 | sed "s/$/\tline/"';
SELECT count(*), min(id), max(id) FROM cp;
BEGIN;
TRUNCATE cp;
COPY cp FROM PROGRAM 'seq 1 500 | sed "s/$/\tfrozen/"' WITH (FREEZE);
COMMIT;
SELECT count(*), max(t) FROM cp;
SELECT live_items = frozen_items AS all_frozen FROM iheap_relation_stats('cp');
COPY (SELECT * FROM cp WHERE id <= 3 ORDER BY id) TO STDOUT;
DROP TABLE cp;
