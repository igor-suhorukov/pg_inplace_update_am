--
-- Logical decoding of iheap tables (DESIGN.md §9.5, §21 D38).  Every
-- change is made to a heap table and to its iheap twin, in the same
-- transaction; the changes decoded for the two must be the same but for
-- the table's name.
--
SELECT 'init' FROM pg_create_logical_replication_slot('ihl', 'test_decoding');

-- types of every kind of layout: by value, fixed lengths of all alignments,
-- varlena, names, arrays, nulls
CREATE TABLE ld_h (
	id int PRIMARY KEY, k int, v text, n numeric, d date, u uuid, b bool,
	c "char", s int2, f float4, i8 int8, nm name, pt point, mac macaddr,
	iv interval, tz timetz, arr int4[], j jsonb) USING heap;
CREATE TABLE ld_i (LIKE ld_h INCLUDING ALL) USING iheap;
CREATE INDEX ld_h_k ON ld_h (k);
CREATE INDEX ld_i_k ON ld_i (k);

-- run a statement (with %1$I for the table) on both tables
CREATE FUNCTION ld_both(sql text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format(sql, 'ld_h');
	EXECUTE format(sql, 'ld_i');
END $$;

SELECT ld_both($$INSERT INTO %1$I VALUES
	(1, 10, 'one', 1.5, '2026-01-01', 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11',
	 true, 'x', 7, 2.5, 9000000000, 'nm1', '(1,2)', '08:00:2b:01:02:03',
	 '1 day 2 hours', '10:00+02', '{1,2,3}', '{"a": 1}'),
	(2, 20, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	 NULL, NULL, NULL, NULL, NULL),
	(3, 30, 'three', -3, '2026-03-03', NULL, false, 'z', -1, 0, -1, 'nm3',
	 '(-1,-2)', NULL, '-1 mon', NULL, '{}', '[1, "x"]')$$);
-- in place: a plain column, then a dmbtree key column
SELECT ld_both($$UPDATE %1$I SET v = 'uno', b = NOT b WHERE id = 1$$);
SELECT ld_both($$UPDATE %1$I SET k = k + 1, n = n * 2 WHERE id = 1$$);
-- a column of the primary key (btree): the row moves, the old key is logged
SELECT ld_both($$UPDATE %1$I SET id = 4 WHERE id = 2$$);
-- a row that grows past its page's room moves too
SELECT ld_both($$UPDATE %1$I SET v = repeat('w', 1000) WHERE id = 3$$);
SELECT ld_both($$DELETE FROM %1$I WHERE id = 1$$);

-- COPY: multi-insert
BEGIN;
COPY ld_h (id, k, v) FROM STDIN;
10	100	copy-a
11	110	copy-b
12	\N	\N
\.
COPY ld_i (id, k, v) FROM STDIN;
10	100	copy-a
11	110	copy-b
12	\N	\N
\.
COMMIT;

-- values stored out of line: inserted, carried along unchanged, replaced
ALTER TABLE ld_h ALTER COLUMN v SET STORAGE EXTERNAL;
ALTER TABLE ld_i ALTER COLUMN v SET STORAGE EXTERNAL;
SELECT ld_both($$INSERT INTO %1$I (id, k, v)
	SELECT g, g, repeat(md5(g::text), 100) FROM generate_series(20, 21) g$$);
SELECT ld_both($$UPDATE %1$I SET k = -k WHERE id = 20$$);
SELECT ld_both($$UPDATE %1$I SET v = repeat(md5('new'), 100) WHERE id = 21$$);

-- ON CONFLICT: a speculative insertion that is confirmed, and one that
-- conflicts and updates instead
SELECT ld_both($$INSERT INTO %1$I (id, k) VALUES (30, 300)
	ON CONFLICT (id) DO NOTHING$$);
SELECT ld_both($$INSERT INTO %1$I (id, k) VALUES (30, 301)
	ON CONFLICT (id) DO UPDATE SET k = excluded.k$$);
SELECT ld_both($$INSERT INTO %1$I (id, k) VALUES (30, 302), (31, 310)
	ON CONFLICT (id) DO NOTHING$$);

-- subtransactions: what was rolled back is not decoded
BEGIN;
SELECT ld_both($$INSERT INTO %1$I (id, k) VALUES (40, 400)$$);
SAVEPOINT s1;
SELECT ld_both($$UPDATE %1$I SET k = 401 WHERE id = 40$$);
SELECT ld_both($$DELETE FROM %1$I WHERE id = 10$$);
ROLLBACK TO s1;
SELECT ld_both($$UPDATE %1$I SET k = 402 WHERE id = 40$$);
SAVEPOINT s2;
SELECT ld_both($$UPDATE %1$I SET k = 403 WHERE id = 40$$);
RELEASE s2;
COMMIT;

-- an aborted transaction leaves nothing
BEGIN;
SELECT ld_both($$UPDATE %1$I SET k = 0$$);
SELECT ld_both($$DELETE FROM %1$I$$);
ROLLBACK;

-- replica identities: the whole old row, then none
ALTER TABLE ld_h REPLICA IDENTITY FULL;
ALTER TABLE ld_i REPLICA IDENTITY FULL;
SELECT ld_both($$UPDATE %1$I SET k = 111 WHERE id = 11$$);
SELECT ld_both($$UPDATE %1$I SET v = 'twenty' WHERE id = 20$$);
SELECT ld_both($$DELETE FROM %1$I WHERE id = 12$$);
ALTER TABLE ld_h REPLICA IDENTITY NOTHING;
ALTER TABLE ld_i REPLICA IDENTITY NOTHING;
SELECT ld_both($$UPDATE %1$I SET id = 41 WHERE id = 40$$);
SELECT ld_both($$DELETE FROM %1$I WHERE id = 41$$);
ALTER TABLE ld_h REPLICA IDENTITY DEFAULT;
ALTER TABLE ld_i REPLICA IDENTITY DEFAULT;

-- dropped and added columns: rows older than the new column lack it
ALTER TABLE ld_h DROP COLUMN j, ADD COLUMN extra int DEFAULT 5;
ALTER TABLE ld_i DROP COLUMN j, ADD COLUMN extra int DEFAULT 5;
SELECT ld_both($$UPDATE %1$I SET k = k + 1000 WHERE id = 3$$);
SELECT ld_both($$INSERT INTO %1$I (id, k, extra) VALUES (50, 500, NULL)$$);

-- rewrites decode to nothing; truncation does
VACUUM FULL ld_h;
VACUUM FULL ld_i;
CLUSTER ld_h USING ld_h_pkey;
CLUSTER ld_i USING ld_i_pkey;
SELECT ld_both($$UPDATE %1$I SET k = k + 1 WHERE id = 50$$);
TRUNCATE ld_h;
TRUNCATE ld_i;

-- partitions, and an update that moves a row to another one
CREATE TABLE ld_hp (id int PRIMARY KEY, v text) PARTITION BY RANGE (id);
CREATE TABLE ld_hp1 PARTITION OF ld_hp FOR VALUES FROM (0) TO (100) USING heap;
CREATE TABLE ld_hp2 PARTITION OF ld_hp FOR VALUES FROM (100) TO (200) USING heap;
CREATE TABLE ld_ip (id int PRIMARY KEY, v text) PARTITION BY RANGE (id);
CREATE TABLE ld_ip1 PARTITION OF ld_ip FOR VALUES FROM (0) TO (100) USING iheap;
CREATE TABLE ld_ip2 PARTITION OF ld_ip FOR VALUES FROM (100) TO (200) USING iheap;
SELECT ld_both($$INSERT INTO %1$Ip VALUES (1, 'a'), (2, 'b')$$);
SELECT ld_both($$UPDATE %1$Ip SET v = 'aa' WHERE id = 1$$);
SELECT ld_both($$UPDATE %1$Ip SET id = 102 WHERE id = 2$$);

-- the changes, in order
CREATE TEMP TABLE dec AS
SELECT ord, data
FROM pg_logical_slot_get_changes('ihl', NULL, NULL, 'include-xids', '0',
								 'skip-empty-xacts', '1')
	 WITH ORDINALITY AS t(lsn, xid, data, ord);

-- the same changes for both access methods
SELECT (SELECT array_agg(regexp_replace(data, '^table public\.ld_h', 'table public.T')
						 ORDER BY ord)
		FROM dec WHERE data ~ '^table public\.ld_h') =
	   (SELECT array_agg(regexp_replace(data, '^table public\.ld_i', 'table public.T')
						 ORDER BY ord)
		FROM dec WHERE data ~ '^table public\.ld_i') AS same_changes,
	   (SELECT count(*) FROM dec WHERE data ~ '^table public\.ld_i') AS iheap_changes;

-- what they are (long values cut short)
SELECT CASE WHEN length(data) > 200
			THEN left(data, 160) || ' ... ' || length(data) || ' chars'
			ELSE data END AS change
FROM dec WHERE data ~ '^table public\.ld_i' ORDER BY ord;

SELECT pg_drop_replication_slot('ihl');
DROP TABLE ld_h, ld_i, ld_hp, ld_ip;
DROP FUNCTION ld_both(text);
