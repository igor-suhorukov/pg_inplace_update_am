--
-- dmbtree: in-place updates of indexed columns (DESIGN.md §11.5, §11.6)
--

-- the access method, with copies of btree's operator classes
SELECT amname, amtype FROM pg_am WHERE amname = 'dmbtree';
SELECT (SELECT count(*) FROM pg_opclass c JOIN pg_am a ON a.oid = c.opcmethod
        WHERE a.amname = 'dmbtree') =
       (SELECT count(*) FROM pg_opclass c JOIN pg_am a ON a.oid = c.opcmethod
        WHERE a.amname = 'btree') AS same_opclasses,
       (SELECT count(*) FROM pg_amop o JOIN pg_am a ON a.oid = o.amopmethod
        WHERE a.amname = 'dmbtree') =
       (SELECT count(*) FROM pg_amop o JOIN pg_am a ON a.oid = o.amopmethod
        WHERE a.amname = 'btree') AS same_operators;
SELECT opcname FROM pg_opclass c JOIN pg_am a ON a.oid = c.opcmethod
WHERE a.amname = 'dmbtree' AND NOT amvalidate(c.oid);
-- with btree's types, defaults and storage types (name_ops stores cstrings)
SELECT b.opcname FROM pg_opclass b
JOIN pg_opclass d ON d.opcname = b.opcname AND d.opcnamespace = b.opcnamespace
WHERE b.opcmethod = (SELECT oid FROM pg_am WHERE amname = 'btree')
  AND d.opcmethod = (SELECT oid FROM pg_am WHERE amname = 'dmbtree')
  AND (b.opcintype, b.opckeytype, b.opcdefault) IS DISTINCT FROM
      (d.opcintype, d.opckeytype, d.opcdefault);

-- ------------------------------------------------------------------------
-- Which updates keep the row in place.  CREATE INDEX without USING on an
-- iheap table makes a dmbtree index (iheap.default_index_am).
-- ------------------------------------------------------------------------
CREATE TABLE dm (id int PRIMARY KEY, k int, s text, b bool, n numeric,
                 inc int, pad text, e int, p int) USING iheap
  WITH (autovacuum_enabled = off);
CREATE INDEX dm_k ON dm (k);
CREATE INDEX dm_ks ON dm (k, s);
CREATE INDEX dm_s ON dm (s);
CREATE INDEX dm_b ON dm (b);
CREATE INDEX dm_n ON dm (n);                    -- numeric: not "equalimage"
CREATE INDEX dm_kinc ON dm (k) INCLUDE (inc);   -- INCLUDE columns
CREATE INDEX dm_pad ON dm USING btree (pad);    -- explicitly btree
CREATE INDEX dm_e ON dm ((e + 1));              -- expression
CREATE INDEX dm_p ON dm (p) WHERE p > 0;        -- partial
SELECT c.relname, a.amname
FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
     JOIN pg_am a ON a.oid = c.relam
WHERE i.indrelid = 'dm'::regclass ORDER BY 1;

INSERT INTO dm SELECT g, g % 100, 's' || g, g % 2 = 0, g, g, 'p', g, g
FROM generate_series(1, 1000) g;
CREATE TEMP TABLE dm_tids AS SELECT id, ctid AS t FROM dm;

-- (keys keep their length: a row that grows needs room on its page)
UPDATE dm SET k = k + 1000 WHERE id BETWEEN 1 AND 100;
UPDATE dm SET s = upper(s) WHERE id BETWEEN 101 AND 200;
UPDATE dm SET b = NOT b WHERE id BETWEEN 201 AND 300;
UPDATE dm SET k = k + 2000, s = 'm' || substr(s, 2) WHERE id BETWEEN 301 AND 400;
UPDATE dm SET n = n + 1 WHERE id BETWEEN 401 AND 500;
UPDATE dm SET inc = inc + 1 WHERE id BETWEEN 501 AND 600;
UPDATE dm SET pad = 'q' WHERE id BETWEEN 601 AND 700;
UPDATE dm SET e = e + 1 WHERE id BETWEEN 701 AND 800;
UPDATE dm SET p = -p WHERE id BETWEEN 801 AND 900;
SELECT CASE WHEN id <= 100 THEN 'key'
            WHEN id <= 200 THEN 'text key'
            WHEN id <= 300 THEN 'bool key'
            WHEN id <= 400 THEN 'two keys'
            WHEN id <= 500 THEN 'numeric key'
            WHEN id <= 600 THEN 'INCLUDE column'
            WHEN id <= 700 THEN 'btree key'
            WHEN id <= 800 THEN 'expression'
            WHEN id <= 900 THEN 'predicate'
            ELSE 'none' END AS updated,
       count(*) FILTER (WHERE dm.ctid = t) AS stayed,
       count(*) FILTER (WHERE dm.ctid <> t) AS moved
FROM dm JOIN dm_tids USING (id)
GROUP BY 1 ORDER BY min(id);

-- an index on the whole row (through a function that is not inlined, so
-- that the expression keeps the whole-row reference): a change of any
-- column changes its key, and the row moves
CREATE TABLE dwr (id int, b text) USING iheap;
CREATE FUNCTION dm_rowkey(r dwr) RETURNS text IMMUTABLE LANGUAGE plpgsql
  AS $$ BEGIN RETURN r.id || ':' || r.b; END $$;
CREATE INDEX dwr_r ON dwr (dm_rowkey(dwr));
INSERT INTO dwr VALUES (1, 'a');
UPDATE dwr SET b = 'x';
SELECT ctid, * FROM dwr;
SET enable_seqscan = off;
SELECT id, b FROM dwr WHERE dm_rowkey(dwr) = '1:x';
SELECT id, b FROM dwr WHERE dm_rowkey(dwr) = '1:a';
RESET enable_seqscan;
DROP TABLE dwr CASCADE;

-- ------------------------------------------------------------------------
-- Every scan finds each row once, through its current key only
-- ------------------------------------------------------------------------
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM dm WHERE k = 1005;
SELECT id FROM dm WHERE k = 1005;
SELECT id FROM dm WHERE k = 5 ORDER BY id;
SELECT count(*), count(DISTINCT id) FROM (SELECT id FROM dm ORDER BY k) x;
SELECT count(*), count(DISTINCT id) FROM (SELECT id FROM dm ORDER BY k DESC) x;
SELECT count(*) FROM dm WHERE k BETWEEN 0 AND 99;
SELECT count(*) FROM dm WHERE k BETWEEN 1000 AND 2999;
SELECT count(*) FROM dm WHERE s LIKE 'm%';
SELECT count(*) FROM dm WHERE s = 'S150';
SELECT count(*) FROM dm WHERE s = 's150';
SELECT count(*) FILTER (WHERE b) AS t, count(*) FILTER (WHERE NOT b) AS f FROM dm;
EXPLAIN (COSTS OFF) SELECT k, s FROM dm WHERE (k, s) >= (2097, 'm397') ORDER BY k, s LIMIT 3;
SELECT k, s FROM dm WHERE (k, s) >= (2097, 'm397') ORDER BY k, s LIMIT 3;
EXPLAIN (COSTS OFF) SELECT k FROM dm WHERE k >= 1000;
SELECT count(k), min(k), max(k) FROM dm WHERE k >= 1000;
SELECT k, inc FROM dm WHERE k = 1001;
RESET enable_bitmapscan;
SET enable_indexscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM dm WHERE k = 1005 OR k = 5 ORDER BY id;
SELECT id FROM dm WHERE k = 1005 OR k = 5 ORDER BY id;
RESET enable_indexscan;
RESET enable_seqscan;

-- the indexes hold delete-marked entries until VACUUM cleans them up
SELECT tids > 1000 AS extra_entries, marked > 0 AS has_marks
FROM dmbtree_index_stats('dm_k');
VACUUM dm;
SELECT tids, marked FROM dmbtree_index_stats('dm_k');
SELECT tids, marked FROM dmbtree_index_stats('dm_ks');
SELECT tids, marked FROM dmbtree_index_stats('dm_s');
SET enable_seqscan = off;
SELECT count(*) FROM dm WHERE k BETWEEN 1000 AND 2999;
RESET enable_seqscan;

-- a key the row had before: its entry is reused, not duplicated
UPDATE dm SET k = 7 WHERE id = 1;
UPDATE dm SET k = 1001 WHERE id = 1;
SELECT tids FROM dmbtree_index_stats('dm_k');
SET enable_seqscan = off;
SELECT id, k FROM dm WHERE k IN (7, 1001) ORDER BY id;
RESET enable_seqscan;

-- rolled back changes leave marked entries behind, never wrong answers
BEGIN;
UPDATE dm SET k = -1 WHERE id BETWEEN 1 AND 10;
ROLLBACK;
BEGIN;
UPDATE dm SET k = -2 WHERE id = 11;
SAVEPOINT a;
UPDATE dm SET k = -3 WHERE id = 11;
UPDATE dm SET k = -3 WHERE id = 13;
ROLLBACK TO a;
UPDATE dm SET k = -4 WHERE id = 12;
COMMIT;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT id, k FROM dm WHERE k < 0 ORDER BY k, id;
SELECT id, k FROM dm WHERE id <= 13 ORDER BY id;
SELECT count(*) FROM dm WHERE k BETWEEN 1001 AND 1013;
RESET enable_bitmapscan;
RESET enable_seqscan;

-- with in-place updates off, rows move and every index gets new entries
SET iheap.enable_inplace = off;
UPDATE dm SET k = k + 1 WHERE id = 20;
RESET iheap.enable_inplace;
SELECT dm.ctid <> t AS moved, k FROM dm JOIN dm_tids USING (id) WHERE id = 20;
SET enable_seqscan = off;
SELECT id FROM dm WHERE k = 1021;
RESET enable_seqscan;

-- ------------------------------------------------------------------------
-- Uniqueness
-- ------------------------------------------------------------------------
CREATE TABLE du (id int, code text, grp int) USING iheap;
CREATE UNIQUE INDEX du_code ON du USING dmbtree (code);
INSERT INTO du VALUES (1, 'a', 1), (2, 'b', 1), (3, NULL, 1), (4, NULL, 1);
CREATE TEMP TABLE du_tids AS SELECT id, ctid AS t FROM du;
UPDATE du SET code = 'b' WHERE id = 1;
UPDATE du SET code = 'c' WHERE id = 1;
UPDATE du SET code = 'a' WHERE id = 2;
UPDATE du SET code = 'c' WHERE id = 3;
UPDATE du SET code = 'd' WHERE id = 3;
UPDATE du SET code = NULL WHERE id = 2;
SELECT id, code, du.ctid = t AS in_place FROM du JOIN du_tids USING (id) ORDER BY id;
-- a key the row itself had is not a duplicate
UPDATE du SET code = 'x' WHERE id = 1;
UPDATE du SET code = 'c' WHERE id = 1;
-- upserts take the dmbtree index as arbiter
INSERT INTO du VALUES (5, 'c', 2) ON CONFLICT (code) DO UPDATE SET grp = du.grp + 10;
INSERT INTO du VALUES (6, 'a', 2) ON CONFLICT (code) DO NOTHING;
INSERT INTO du VALUES (7, 'x', 2) ON CONFLICT (code) DO NOTHING;
SELECT id, code, grp FROM du ORDER BY id;
CREATE TABLE dnn (id int, v int) USING iheap;
CREATE UNIQUE INDEX dnn_v ON dnn USING dmbtree (v) NULLS NOT DISTINCT;
INSERT INTO dnn VALUES (1, 1), (2, NULL);
UPDATE dnn SET v = NULL WHERE id = 1;
UPDATE dnn SET v = 2 WHERE id = 2;
UPDATE dnn SET v = NULL WHERE id = 1;
SELECT * FROM dnn ORDER BY id;

-- unique indexes and constraints: iheap.unique_index_am (btree by default)
CREATE TABLE dui (id int, a int) USING iheap;
CREATE UNIQUE INDEX dui_a ON dui (a);
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE relname = 'dui_a';
DROP TABLE dui;
SET iheap.unique_index_am = dmbtree;
CREATE TABLE dc (id int PRIMARY KEY, a int UNIQUE, b int,
                 UNIQUE (b) DEFERRABLE INITIALLY DEFERRED) USING iheap;
ALTER TABLE dc ADD COLUMN c int UNIQUE;
RESET iheap.unique_index_am;
CREATE TABLE dc2 (id int PRIMARY KEY) USING iheap;
SELECT c.relname, a.amname
FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
     JOIN pg_am a ON a.oid = c.relam
WHERE i.indrelid IN ('dc'::regclass, 'dc2'::regclass) ORDER BY 1;
INSERT INTO dc VALUES (1, 1, 1, 1), (2, 2, 2, 2);
CREATE TEMP TABLE dc_tids AS SELECT id, ctid AS t FROM dc;
UPDATE dc SET a = a + 10 WHERE id = 1;      -- immediate unique: in place
UPDATE dc SET b = b + 10 WHERE id = 2;      -- deferrable unique: moves
SELECT id, a, b, dc.ctid = t AS in_place FROM dc JOIN dc_tids USING (id) ORDER BY id;
UPDATE dc SET id = id + 100 WHERE id = 1;   -- primary key: in place
SELECT dc.id, dc.ctid = t AS in_place FROM dc JOIN dc_tids ON dc_tids.id + 100 = dc.id;
UPDATE dc SET a = 2 WHERE id = 101;
BEGIN;
UPDATE dc SET b = 1 WHERE id = 101;
UPDATE dc SET b = 1 WHERE id = 2;
COMMIT;
SELECT * FROM dc ORDER BY id;

-- an exclusion constraint's index is never updated in place
CREATE TABLE dx (id int, r int, EXCLUDE USING dmbtree (r WITH =)) USING iheap;
INSERT INTO dx VALUES (1, 1), (2, 2);
CREATE TEMP TABLE dx_tids AS SELECT id, ctid AS t FROM dx;
UPDATE dx SET r = 3 WHERE id = 1;
UPDATE dx SET r = 3 WHERE id = 2;
SELECT id, r, dx.ctid = t AS in_place FROM dx JOIN dx_tids USING (id) ORDER BY id;

-- ------------------------------------------------------------------------
-- Index builds on tables with history, rewrites
-- ------------------------------------------------------------------------
CREATE TABLE dbld (id int, v int, w text) USING iheap;
INSERT INTO dbld SELECT g, g, 'w' || g FROM generate_series(1, 2000) g;
CREATE INDEX dbld_v ON dbld (v);
UPDATE dbld SET v = v + 10000 WHERE id % 3 = 0;
DELETE FROM dbld WHERE id % 5 = 0;
CREATE INDEX CONCURRENTLY dbld_w ON dbld (w);
UPDATE dbld SET w = w || 'x' WHERE id % 7 = 0;
CREATE UNIQUE INDEX CONCURRENTLY dbld_id ON dbld (id);
REINDEX INDEX dbld_v;
REINDEX INDEX CONCURRENTLY dbld_w;
SET enable_seqscan = off;
SELECT count(*) FROM dbld WHERE v > 10000;
SELECT count(*) FROM dbld WHERE w LIKE '%x';
SELECT count(*) FROM dbld WHERE id = 21;
RESET enable_seqscan;
CLUSTER dbld USING dbld_v;
UPDATE dbld SET v = v + 1 WHERE id < 10;
VACUUM FULL dbld;
SET enable_seqscan = off;
SELECT count(*), count(DISTINCT id) FROM (SELECT id FROM dbld ORDER BY v) x;
SELECT id, v FROM dbld WHERE id < 10 ORDER BY v;
RESET enable_seqscan;
SELECT count(*) FROM dbld;
-- a parallel build
SET max_parallel_maintenance_workers = 2;
SET min_parallel_table_scan_size = 0;
CREATE INDEX dbld_vw ON dbld (v, w);
RESET max_parallel_maintenance_workers;
RESET min_parallel_table_scan_size;
SET enable_seqscan = off;
SELECT count(*) FROM dbld WHERE v BETWEEN 100 AND 200;
RESET enable_seqscan;

-- ------------------------------------------------------------------------
-- dmbtree on a heap table behaves as btree
-- ------------------------------------------------------------------------
CREATE TABLE dhp (id int, k int);
CREATE INDEX dhp_k ON dhp USING dmbtree (k);
INSERT INTO dhp SELECT g, g FROM generate_series(1, 1000) g;
UPDATE dhp SET k = k + 1 WHERE id <= 10;
DELETE FROM dhp WHERE id BETWEEN 500 AND 509;
VACUUM dhp;
SET enable_seqscan = off;
SELECT count(*) FROM dhp WHERE k BETWEEN 2 AND 11;
SELECT count(*) FROM dhp WHERE k BETWEEN 500 AND 509;
RESET enable_seqscan;
SELECT tids, marked FROM dmbtree_index_stats('dhp_k');

-- ------------------------------------------------------------------------
-- Partitioned, temporary and unlogged tables; TOASTed keys
-- ------------------------------------------------------------------------
CREATE TABLE dpt (id int, k int) PARTITION BY RANGE (id) USING iheap;
CREATE TABLE dpt1 PARTITION OF dpt FOR VALUES FROM (0) TO (500);
CREATE TABLE dpt2 PARTITION OF dpt FOR VALUES FROM (500) TO (1000);
CREATE INDEX dpt_k ON dpt (k);
SELECT c.relname, a.amname
FROM pg_class c JOIN pg_am a ON a.oid = c.relam
WHERE c.relname LIKE 'dpt%k%' ORDER BY 1;
INSERT INTO dpt SELECT g, g FROM generate_series(0, 999) g;
CREATE TEMP TABLE dpt_tids AS SELECT id, ctid AS t, tableoid AS part FROM dpt;
UPDATE dpt SET k = k + 5000 WHERE id % 10 = 0;
SELECT count(*) FILTER (WHERE dpt.ctid = t AND dpt.tableoid = part) AS stayed
FROM dpt JOIN dpt_tids USING (id)
WHERE id % 10 = 0;
SET enable_seqscan = off;
SELECT count(*) FROM dpt WHERE k >= 5000;
RESET enable_seqscan;

CREATE TEMP TABLE dtmp (id int, k int) USING iheap;
CREATE INDEX dtmp_k ON dtmp (k);
INSERT INTO dtmp SELECT g, g FROM generate_series(1, 500) g;
UPDATE dtmp SET k = -k WHERE id <= 50;
CREATE UNLOGGED TABLE dunl (id int, k int) USING iheap;
CREATE INDEX dunl_k ON dunl (k);
INSERT INTO dunl SELECT g, g FROM generate_series(1, 500) g;
UPDATE dunl SET k = -k WHERE id <= 50;
SET enable_seqscan = off;
SELECT (SELECT count(*) FROM dtmp WHERE k < 0) AS tmp, (SELECT count(*) FROM dunl WHERE k < 0) AS unl;
RESET enable_seqscan;
VACUUM dtmp, dunl;
SELECT (SELECT marked FROM dmbtree_index_stats('dtmp_k')) AS tmp_marked,
       (SELECT marked FROM dmbtree_index_stats('dunl_k')) AS unl_marked;

-- keys stored out of line in the table (incompressible, ~2.4 kB)
CREATE FUNCTION dm_bigkey(int) RETURNS text IMMUTABLE LANGUAGE sql
  AS $$ SELECT string_agg(md5($1::text || ':' || i), '') FROM generate_series(1, 75) i $$;
CREATE TABLE dtoast (id int, t text) USING iheap;
CREATE INDEX dtoast_t ON dtoast (t);
INSERT INTO dtoast SELECT g, dm_bigkey(g) FROM generate_series(1, 20) g;
SELECT count(*) AS external FROM dtoast WHERE pg_column_toast_chunk_id(t) IS NOT NULL;
CREATE TEMP TABLE dtoast_tids AS SELECT id, ctid AS t FROM dtoast;
UPDATE dtoast SET t = dm_bigkey(id + 100) WHERE id <= 10;
SELECT count(*) FILTER (WHERE dtoast.ctid = dtoast_tids.t) AS stayed
FROM dtoast JOIN dtoast_tids USING (id) WHERE id <= 10;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT id FROM dtoast WHERE t = dm_bigkey(105);
SELECT id FROM dtoast WHERE t = dm_bigkey(5);
SELECT id FROM dtoast WHERE t = dm_bigkey(15);
SELECT count(*) FROM dtoast WHERE t IN (dm_bigkey(5), dm_bigkey(105));
-- an ON CONFLICT probe checks the keys of the rows it finds
CREATE UNIQUE INDEX dtoast_u ON dtoast USING dmbtree (t);
INSERT INTO dtoast VALUES (99, dm_bigkey(105)) ON CONFLICT (t) DO NOTHING;
INSERT INTO dtoast VALUES (98, dm_bigkey(5)) ON CONFLICT (t) DO NOTHING;
SELECT id FROM dtoast WHERE t IN (dm_bigkey(5), dm_bigkey(105)) ORDER BY id;
DELETE FROM dtoast WHERE id = 98;
DROP INDEX dtoast_u;
RESET enable_bitmapscan;
RESET enable_seqscan;
VACUUM dtoast;
SELECT tids, marked FROM dmbtree_index_stats('dtoast_t');

-- ------------------------------------------------------------------------
-- Operator classes
-- ------------------------------------------------------------------------
-- name keys are stored as cstrings, as in btree, and index-only scans pad
-- them out again
CREATE TABLE dnm (id int, n name) USING iheap;
CREATE INDEX dnm_n ON dnm (n);
SELECT atttypid::regtype FROM pg_attribute WHERE attrelid = 'dnm_n'::regclass;
INSERT INTO dnm SELECT g, 'name' || g FROM generate_series(1, 100) g;
UPDATE dnm SET n = n || 'x' WHERE id <= 3;
VACUUM dnm;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT n FROM dnm WHERE n IN ('name1', 'name1x', 'name4');
SELECT n, octet_length(n::text) FROM dnm WHERE n IN ('name1', 'name1x', 'name4') ORDER BY n;
RESET enable_bitmapscan;
RESET enable_seqscan;

-- pattern classes and nondeterministic collations: refused as btree does
CREATE COLLATION dm_ci (provider = icu, locale = 'und-u-ks-level2', deterministic = false);
CREATE TABLE dci (x text COLLATE dm_ci) USING iheap;
CREATE INDEX ON dci (x text_pattern_ops);
CREATE INDEX ON dci USING dmbtree (x text_pattern_ops);
CREATE INDEX ON dci USING dmbtree (x COLLATE "C" text_pattern_ops);
CREATE INDEX dci_x ON dci (x);
INSERT INTO dci VALUES ('abc'), ('ABC'), ('def');
SET enable_seqscan = off;
SELECT x FROM dci WHERE x = 'abc' ORDER BY x COLLATE "C";
RESET enable_seqscan;

-- a btree class created after the extension: dmbtree has no copy, so the
-- index stays btree until a superuser copies the class
CREATE OPERATOR FAMILY dm_fam USING btree;
CREATE OPERATOR CLASS dm_int_ops FOR TYPE int4 USING btree FAMILY dm_fam AS
  OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >,
  FUNCTION 1 btint4cmp(int4, int4);
CREATE TABLE dop (id int, k int) USING iheap;
CREATE INDEX dop_k1 ON dop (k dm_int_ops);
SELECT dmbtree_copy_btree_opclasses();
SELECT dmbtree_copy_btree_opclasses();
CREATE INDEX dop_k2 ON dop (k dm_int_ops);
CREATE INDEX dop_e ON dop ((k + 1));
-- what btree refuses is refused by btree
CREATE INDEX dop_p ON dop (point(k, k));
CREATE INDEX dop_n ON dop (k no_such_ops);
-- constraints, with iheap.unique_index_am = dmbtree
SET iheap.unique_index_am = dmbtree;
ALTER TABLE dop ADD COLUMN u serial UNIQUE, ADD CONSTRAINT dop_pk PRIMARY KEY (id);
CREATE TABLE dop2 (id bigserial PRIMARY KEY, k int UNIQUE) USING iheap;
RESET iheap.unique_index_am;
SELECT c.relname, a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
WHERE c.relname ~ '^dop' AND c.relkind = 'i' ORDER BY 1;
-- btree's family gains an operator its copy lacks: the planner keeps to
-- the copy's operators until it is made again
CREATE TABLE dop3 (k int) USING iheap;
CREATE INDEX dop3_k ON dop3 (k dm_int_ops);
INSERT INTO dop3 SELECT generate_series(1, 10);
ALTER OPERATOR FAMILY dm_fam USING btree ADD
  OPERATOR 3 = (int4, int8), FUNCTION 1 (int4, int8) btint48cmp(int4, int8);
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT k FROM dop3 WHERE k = 5::int8;
SELECT k FROM dop3 WHERE k = 5::int8;
SELECT dmbtree_copy_btree_opclasses();
EXPLAIN (COSTS OFF) SELECT k FROM dop3 WHERE k = 5::int8;
SELECT k FROM dop3 WHERE k = 5::int8;
RESET enable_seqscan;

-- a type whose only btree class is newer than the extension: its indexes
-- and constraints stay btree until the class is copied
CREATE FUNCTION dm_pcmp(point, point) RETURNS int IMMUTABLE STRICT LANGUAGE sql
  AS $$ SELECT CASE WHEN $1[0] <> $2[0] THEN sign($1[0] - $2[0])::int
                    ELSE sign($1[1] - $2[1])::int END $$;
CREATE FUNCTION dm_plt(point, point) RETURNS bool IMMUTABLE STRICT LANGUAGE sql
  AS $$ SELECT dm_pcmp($1, $2) < 0 $$;
CREATE FUNCTION dm_ple(point, point) RETURNS bool IMMUTABLE STRICT LANGUAGE sql
  AS $$ SELECT dm_pcmp($1, $2) <= 0 $$;
CREATE FUNCTION dm_peq(point, point) RETURNS bool IMMUTABLE STRICT LANGUAGE sql
  AS $$ SELECT dm_pcmp($1, $2) = 0 $$;
CREATE FUNCTION dm_pge(point, point) RETURNS bool IMMUTABLE STRICT LANGUAGE sql
  AS $$ SELECT dm_pcmp($1, $2) >= 0 $$;
CREATE FUNCTION dm_pgt(point, point) RETURNS bool IMMUTABLE STRICT LANGUAGE sql
  AS $$ SELECT dm_pcmp($1, $2) > 0 $$;
CREATE OPERATOR #< (LEFTARG = point, RIGHTARG = point, FUNCTION = dm_plt);
CREATE OPERATOR #<= (LEFTARG = point, RIGHTARG = point, FUNCTION = dm_ple);
CREATE OPERATOR #= (LEFTARG = point, RIGHTARG = point, FUNCTION = dm_peq);
CREATE OPERATOR #>= (LEFTARG = point, RIGHTARG = point, FUNCTION = dm_pge);
CREATE OPERATOR #> (LEFTARG = point, RIGHTARG = point, FUNCTION = dm_pgt);
CREATE OPERATOR CLASS dm_point_ops DEFAULT FOR TYPE point USING btree AS
  OPERATOR 1 #<, OPERATOR 2 #<=, OPERATOR 3 #=, OPERATOR 4 #>=, OPERATOR 5 #>,
  FUNCTION 1 dm_pcmp(point, point);
SET iheap.unique_index_am = dmbtree;
CREATE TABLE dpq (p point PRIMARY KEY, q point) USING iheap;
ALTER TABLE dpq ADD CONSTRAINT dpq_q UNIQUE (q);
CREATE INDEX dpq_qi ON dpq (q);
SELECT dmbtree_copy_btree_opclasses();
CREATE INDEX dpq_qi2 ON dpq (q);
ALTER TABLE dpq ADD COLUMN r point UNIQUE;
RESET iheap.unique_index_am;
SELECT c.relname, a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
WHERE c.relname ~ '^dpq' AND c.relkind = 'i' ORDER BY 1;
INSERT INTO dpq VALUES ('(1,1)', '(2,2)', '(3,3)');
INSERT INTO dpq VALUES ('(1,1)', '(4,4)', '(5,5)');
DROP TABLE dpq;
DROP OPERATOR FAMILY dm_point_ops USING dmbtree;
DROP OPERATOR FAMILY dm_point_ops USING btree;
DROP OPERATOR #< (point, point), #<= (point, point), #= (point, point),
  #>= (point, point), #> (point, point);
DROP FUNCTION dm_plt(point, point), dm_ple(point, point), dm_peq(point, point),
  dm_pge(point, point), dm_pgt(point, point), dm_pcmp(point, point);

-- not a dmbtree index
SELECT * FROM dmbtree_index_stats('dm_pad');

DROP TABLE dm, du, dnn, dc, dc2, dx, dbld, dhp, dpt, dtmp, dunl, dtoast,
  dnm, dci, dop, dop2, dop3;
DROP FUNCTION dm_bigkey(int);
DROP OPERATOR FAMILY dm_fam USING dmbtree;
DROP OPERATOR FAMILY dm_fam USING btree;
DROP COLLATION dm_ci;
