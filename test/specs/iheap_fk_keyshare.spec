# Foreign keys and KEY SHARE locks against concurrent updates of the
# referenced row.  As with heap, a non-key update does not block inserts of
# referencing rows; the KEY SHARE lock then survives a rollback of that
# update.  A key update does block them.  The same holds for a non-key
# update that moves the row (it changes an indexed column, "tag"): the lock
# is taken on both homes of the row.

setup
{
  CREATE TABLE parent (id int PRIMARY KEY, aux text, tag int) USING iheap;
  CREATE INDEX parent_tag ON parent (tag);
  CREATE TABLE child (id int PRIMARY KEY, pid int REFERENCES parent) USING iheap;
  INSERT INTO parent VALUES (1, 'a', 0), (2, 'b', 0);
}

teardown
{
  DROP TABLE child, parent;
}

session s1
step s1b    { BEGIN; }
step s1aux  { UPDATE parent SET aux = 'changed' WHERE id = 1; }
step s1key  { UPDATE parent SET id = 10 WHERE id = 1; }
step s1tag  { UPDATE parent SET tag = tag + 1 WHERE id = 1; }
step s1c    { COMMIT; }
step s1a    { ROLLBACK; }

session s2
step s2b    { BEGIN; }
step s2ins  { INSERT INTO child VALUES (100, 1); }
step s2c    { COMMIT; }

session s3
step s3del  { DELETE FROM parent WHERE id = 1; }
step s3sel  { SELECT p.id, p.aux, p.tag, (SELECT count(*) FROM child c WHERE c.pid = p.id) AS children FROM parent p ORDER BY p.id; }

# non-key update: the referencing insert does not wait
permutation s1b s1aux s2b s2ins s2c s1c s3sel
permutation s1b s1aux s2b s2ins s1c s2c s3sel
# the update rolls back while the KEY SHARE lock is held: the lock stays,
# so a delete of the parent waits for it, then fails on the new child
permutation s1b s1aux s2b s2ins s1a s3del s2c s3sel
# key update: the referencing insert waits, then fails (the key is gone)
permutation s1b s1key s2b s2ins s1c s2c s3sel
permutation s1b s1key s2b s2ins s1a s2c s3sel
# non-key update that moves the row: no waiting either; the lock holds on
# the new home if the move commits, on the old one if it rolls back
permutation s1b s1tag s2b s2ins s2c s1c s3sel
permutation s1b s1tag s2b s2ins s1c s3del s2c s3sel
permutation s1b s1tag s2b s2ins s1a s3del s2c s3sel
