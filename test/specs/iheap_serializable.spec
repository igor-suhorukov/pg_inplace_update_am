# SERIALIZABLE (SSI) on iheap: predicate locks taken by index, bitmap and
# sequential scans, conflicts detected on in-place updates, and failures
# raised in the middle of a scan (the abort must then roll back the
# transaction's iheap changes without deadlocking on the scan's buffers).

setup
{
  CREATE TABLE accounts (accountid text NOT NULL PRIMARY KEY, balance numeric NOT NULL) USING iheap;
  INSERT INTO accounts VALUES ('checking', 600), ('savings', 600);
  CREATE TABLE bank_account (id text PRIMARY KEY, balance numeric NOT NULL) USING iheap;
  INSERT INTO bank_account VALUES ('X', 0), ('Y', 0);
}

teardown
{
  DROP TABLE accounts, bank_account;
}

session s1
setup       { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step wx1    { UPDATE accounts SET balance = balance - 200 WHERE accountid = 'checking'; }
step rxy1   { SELECT SUM(balance) FROM accounts; }
step s1ry   { SELECT balance FROM bank_account WHERE id = 'Y'; }
step s1wy   { UPDATE bank_account SET balance = 20 WHERE id = 'Y'; }
step c1     { COMMIT; }

session s2
setup       { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step wy2    { UPDATE accounts SET balance = balance - 200 WHERE accountid = 'savings'; }
step rxy2   { SELECT SUM(balance) FROM accounts; }
step s2rx   { SELECT balance FROM bank_account WHERE id = 'X'; }
step s2ry   { SELECT balance FROM bank_account WHERE id = 'Y'; }
step s2wx   { UPDATE bank_account SET balance = -11 WHERE id = 'X'; }
step c2     { COMMIT; }

session s3
setup       { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s3r    { SELECT id, balance FROM bank_account WHERE id IN ('X', 'Y') ORDER BY id; }
step c3     { COMMIT; }

session s4
step s4sel  { SELECT * FROM accounts ORDER BY accountid; }
step s4bank { SELECT * FROM bank_account ORDER BY id; }

# write skew: one of the two must fail (sometimes while reading)
permutation wx1 rxy1 c1 wy2 rxy2 c2 s4sel
permutation wx1 rxy1 wy2 c1 rxy2 c2 s4sel
permutation wx1 wy2 rxy1 rxy2 c1 c2 s4sel
permutation wx1 wy2 rxy2 rxy1 c2 c1 s4sel
# read-only anomaly: the pivot fails when the read-only transaction has seen
# the committed write
permutation s2rx s2ry s1ry s1wy c1 s3r c3 s2wx c2 s4bank
