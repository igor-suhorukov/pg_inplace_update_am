# Rollback races forced with injection points: the aborting backend's own
# rollback held after everybody sees the abort, while other writers roll back
# and change the rows, while another writer's change on top rolls back too,
# while VACUUM's discard applies the same records, and for a subtransaction
# while a writer waits for the transaction above it; and discard held between
# collecting the targets of aborted records and applying them, while a writer
# changes one.  Each must leave the rows as their committed versions say.
# Needs a build with injection points.

setup
{
  CREATE EXTENSION IF NOT EXISTS injection_points;
  CREATE TABLE rr (id int PRIMARY KEY, v int, s text) USING iheap;
  INSERT INTO rr SELECT g, g, 's' || g FROM generate_series(1, 10) g;
}

teardown
{
  DROP TABLE rr;
}

# its rollbacks wait at iheap-eager-rollback until woken
session a
setup
{
  SELECT injection_points_set_local();
  SELECT injection_points_attach('iheap-eager-rollback', 'wait');
}
step a_begin	{ BEGIN; }
step a_change	{ UPDATE rr SET v = v + 100, s = repeat('a', 100) WHERE id <= 3; DELETE FROM rr WHERE id = 4; INSERT INTO rr VALUES (11, 11, 'a'); }
step a_rollback	{ ROLLBACK; }
step a_sub		{ BEGIN; UPDATE rr SET v = v + 1 WHERE id = 5; SAVEPOINT s; UPDATE rr SET v = v + 1000, s = repeat('a', 200) WHERE id = 5; }
step a_rollback_sub	{ ROLLBACK TO SAVEPOINT s; }
step a_commit	{ COMMIT; }
teardown	{ SELECT injection_points_detach('iheap-eager-rollback'); }

session b
step b_change	{ UPDATE rr SET v = v + 1 WHERE id <= 4; INSERT INTO rr VALUES (11, 1100, 'b'); }
step b_begin	{ BEGIN; }
step b_change2	{ UPDATE rr SET v = v * 2, s = 'b' WHERE id = 1; }
step b_rollback	{ ROLLBACK; }
step b_upd5		{ UPDATE rr SET v = v + 10 WHERE id = 5; }
step b_lazy		{ SET iheap.foreground_undo_limit = 0; BEGIN; UPDATE rr SET v = -v WHERE id = 6; ROLLBACK; RESET iheap.foreground_undo_limit; }
step b_upd6		{ UPDATE rr SET v = v + 1 WHERE id = 6; }
step b_wake_discard	{ SELECT injection_points_wakeup('iheap-discard-apply'); }

session c
step c_sel		{ SELECT id, v, length(s) AS len FROM rr ORDER BY id; }
step c_vacuum	{ VACUUM rr; }
step c_wake		{ SELECT injection_points_wakeup('iheap-eager-rollback'); }
step c_check	{ SELECT count(*) AS problems FROM iheap_check('rr'); }

# its discards wait at iheap-discard-apply until woken
session d
setup
{
  SELECT injection_points_set_local();
  SELECT injection_points_attach('iheap-discard-apply', 'wait');
}
step d_vacuum	{ VACUUM rr; }
teardown	{ SELECT injection_points_detach('iheap-discard-apply'); }

# writers roll back and change the rows; the held rollback leaves them be
permutation a_begin a_change a_rollback c_sel b_change c_sel c_wake c_sel c_check
# another writer's change on top of them rolls back meanwhile
permutation a_begin a_change a_rollback b_begin b_change2 b_rollback c_wake c_sel c_check
# discard applies the records first
permutation a_begin a_change a_rollback c_vacuum c_wake c_sel c_check
# a subtransaction's: the writer then waits for the transaction above it
permutation a_sub a_rollback_sub b_upd5 c_sel c_wake a_commit c_sel c_check
# discard held before applying an aborted record whose row changes meanwhile
permutation b_lazy d_vacuum b_upd6 b_wake_discard c_sel c_check
