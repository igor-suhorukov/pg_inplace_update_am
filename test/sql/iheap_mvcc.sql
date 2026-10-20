--
-- Rollback, savepoints and snapshots within one session
--
CREATE TABLE im (id int PRIMARY KEY, v int, s text) USING iheap;
INSERT INTO im SELECT g, g, 'v' || g FROM generate_series(1, 20) g;

-- rollback of each kind of change
BEGIN;
INSERT INTO im VALUES (21, 21, 'new');
UPDATE im SET v = -v WHERE id <= 5;                 -- in place
UPDATE im SET s = repeat('x', 300) WHERE id = 6;    -- in place, grows
UPDATE im SET id = id + 100 WHERE id = 7;           -- moves
DELETE FROM im WHERE id = 8;
SELECT count(*), sum(v) FROM im;
ROLLBACK;
SELECT count(*), sum(v), max(length(s)) FROM im;
SELECT * FROM im WHERE id IN (5, 6, 7, 8, 107) ORDER BY id;

-- savepoints: partial rollback keeps earlier changes of the transaction
BEGIN;
UPDATE im SET v = 100 WHERE id = 1;
SAVEPOINT a;
UPDATE im SET v = 200 WHERE id = 1;
UPDATE im SET v = 200 WHERE id = 2;
DELETE FROM im WHERE id = 3;
INSERT INTO im VALUES (30, 30, 'sp');
SAVEPOINT b;
UPDATE im SET v = 300 WHERE id = 1;
ROLLBACK TO b;
SELECT id, v FROM im WHERE id IN (1, 2, 3, 30) ORDER BY id;
ROLLBACK TO a;
SELECT id, v FROM im WHERE id IN (1, 2, 3, 30) ORDER BY id;
UPDATE im SET v = 101 WHERE id = 1;
COMMIT;
SELECT id, v FROM im WHERE id IN (1, 2, 3, 30) ORDER BY id;

-- an error inside a subtransaction (PL/pgSQL exception block)
DO $$
BEGIN
  UPDATE im SET v = 999 WHERE id = 10;
  BEGIN
    UPDATE im SET v = 888 WHERE id = 11;
    DELETE FROM im WHERE id = 12;
    PERFORM 1 / 0;
  EXCEPTION WHEN division_by_zero THEN
    RAISE NOTICE 'caught';
  END;
END $$;
SELECT id, v FROM im WHERE id IN (10, 11, 12) ORDER BY id;

-- many subtransactions touching the same row
BEGIN;
SELECT format('SAVEPOINT s%s; UPDATE im SET v = v + 1 WHERE id = 13;%s', g,
              CASE WHEN g % 2 = 0 THEN ' ROLLBACK TO s' || g || ';' ELSE '' END)
FROM generate_series(1, 100) g \gexec
SELECT v FROM im WHERE id = 13;
COMMIT;
SELECT v FROM im WHERE id = 13;

-- a cursor keeps its snapshot while the rows change underneath
BEGIN;
DECLARE c CURSOR FOR SELECT id, v FROM im WHERE id <= 4 ORDER BY id;
FETCH 1 FROM c;
UPDATE im SET v = v * 10 WHERE id <= 4;
FETCH ALL FROM c;
SELECT id, v FROM im WHERE id <= 4 ORDER BY id;
COMMIT;

-- command ids: a statement does not see its own changes
UPDATE im SET v = v + (SELECT max(v) FROM im) WHERE id = 14;
SELECT v FROM im WHERE id = 14;

-- REPEATABLE READ sees its snapshot; updates by itself are visible
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT sum(v) FROM im;
UPDATE im SET v = 0 WHERE id = 15;
SELECT v FROM im WHERE id = 15;
COMMIT;

-- WITH HOLD cursor survives the commit of the updates it must not see
BEGIN;
DECLARE h CURSOR WITH HOLD FOR SELECT id, v FROM im WHERE id IN (16, 17) ORDER BY id;
COMMIT;
UPDATE im SET v = -1 WHERE id IN (16, 17);
FETCH ALL FROM h;
CLOSE h;

-- row locks by ourselves, then modification
BEGIN;
SELECT id FROM im WHERE id = 18 FOR KEY SHARE;
SELECT id FROM im WHERE id = 18 FOR SHARE;
SELECT id FROM im WHERE id = 18 FOR NO KEY UPDATE;
SELECT id FROM im WHERE id = 18 FOR UPDATE;
UPDATE im SET v = 1818 WHERE id = 18;
SELECT id, v FROM im WHERE id = 18;
ROLLBACK;
SELECT id, v FROM im WHERE id = 18;

-- locking then rolling back to a savepoint
BEGIN;
SAVEPOINT x;
SELECT id FROM im WHERE id = 19 FOR UPDATE;
ROLLBACK TO x;
UPDATE im SET v = 1919 WHERE id = 19;
COMMIT;
SELECT id, v FROM im WHERE id = 19;

-- prepared transactions
BEGIN;
UPDATE im SET v = 2020 WHERE id = 20;
INSERT INTO im VALUES (40, 40, 'prepared');
PREPARE TRANSACTION 'im1';
SELECT id, v FROM im WHERE id IN (20, 40) ORDER BY id;
ROLLBACK PREPARED 'im1';
SELECT id, v FROM im WHERE id IN (20, 40) ORDER BY id;
BEGIN;
UPDATE im SET v = 2021 WHERE id = 20;
PREPARE TRANSACTION 'im2';
COMMIT PREPARED 'im2';
SELECT id, v FROM im WHERE id = 20;

-- the abort callback leaves big transactions to lazy rollback
SET iheap.foreground_undo_limit = 0;
BEGIN;
UPDATE im SET v = v + 1000;
DELETE FROM im WHERE id > 15;
ROLLBACK;
RESET iheap.foreground_undo_limit;
SELECT count(*), sum(v) FROM im;
UPDATE im SET v = v + 1 WHERE id = 1;     -- writer rolls back first
SELECT count(*), sum(v) FROM im;
VACUUM im;
SELECT count(*), sum(v) FROM im;

-- final state
SELECT * FROM im ORDER BY id;
DROP TABLE im;
