/*-------------------------------------------------------------------------
 *
 * iheap_stats.c
 *	  Cumulative statistics of iheap relations: a custom pgstat kind.
 *
 * One entry per iheap relation, keyed by database and relation OID, counts
 *	- updates kept in place, and updates that moved the row, by the reason
 *	  (a changed column of an index that cannot take it, a trigger that needs
 *	  the old row by TID, no room on the page, the setting);
 *	- undo written, rolled back (by the aborting backend, lazily by later
 *	  writers and pruning, or by discard's verification) and discarded;
 *	- the walks readers make through undo to older versions.
 *
 * Counts go to backend-local pending entries (pgstat_prep_pending_entry),
 * which the backend flushes to shared memory when it reports its
 * statistics, as core does with its relation counters.  Nothing is counted
 * inside a critical section: rollback adds up what it applies in a tally,
 * which its caller reports.  The entry of a relation is dropped with the
 * relation (an object access hook), and when a transaction that created
 * the relation aborts.
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/pgstat_internal.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "iheap.h"

/*
 * The kind ID for development, until one is reserved on the wiki's list of
 * custom cumulative statistics.
 */
#define IHEAP_STAT_KIND		PGSTAT_KIND_EXPERIMENTAL

typedef struct IHeapStatCounts
{
	PgStat_Counter inplace_updates;
	PgStat_Counter inplace_key_updates; /* ... that changed dmbtree keys */
	PgStat_Counter moved_updates[IHEAP_MOVE_NREASONS];	/* by reason */
	PgStat_Counter undo_records;	/* undo records written */
	PgStat_Counter undo_bytes;
	PgStat_Counter rollback_records[IHEAP_ROLLBACK_NKINDS];	/* applied */
	PgStat_Counter rollback_bytes;
	PgStat_Counter discarded_pages;
	PgStat_Counter discarded_bytes;
	PgStat_Counter walks;		/* versions rebuilt from undo by readers */
	PgStat_Counter walk_steps;	/* older versions rebuilt by those walks */
	PgStat_Counter walk_max_steps;	/* longest walk */
	TimestampTz stat_reset_timestamp;	/* shared entries only */
} IHeapStatCounts;

typedef struct PgStatShared_IHeap
{
	PgStatShared_Common header;
	IHeapStatCounts stats;
} PgStatShared_IHeap;

static bool iheap_stat_flush_pending(PgStat_EntryRef *entry_ref, bool nowait);
static void iheap_stat_reset_timestamp(PgStatShared_Common *header,
									   TimestampTz ts);

static const PgStat_KindInfo iheap_stat_kind = {
	.name = "iheap",
	.fixed_amount = false,
	.accessed_across_databases = false,
	.write_to_file = true,
	.shared_size = sizeof(PgStatShared_IHeap),
	.shared_data_off = offsetof(PgStatShared_IHeap, stats),
	.shared_data_len = sizeof(((PgStatShared_IHeap *) 0)->stats),
	.pending_size = sizeof(IHeapStatCounts),
	.flush_pending_cb = iheap_stat_flush_pending,
	.reset_timestamp_cb = iheap_stat_reset_timestamp,
};

/* undo applied by rollback since its caller last reported it */
static PgStat_Counter rollback_tally_records = 0;
static PgStat_Counter rollback_tally_bytes = 0;

void
iheap_stats_init(void)
{
	pgstat_register_kind(IHEAP_STAT_KIND, &iheap_stat_kind);
}

/* the pending counters of relation relid, or NULL if not counting */
static IHeapStatCounts *
pending_counts(Oid relid)
{
	PgStat_EntryRef *entry_ref;

	if (!pgstat_track_counts || !OidIsValid(relid) ||
		!OidIsValid(MyDatabaseId))
		return NULL;
	Assert(CritSectionCount == 0);

	entry_ref = pgstat_prep_pending_entry(IHEAP_STAT_KIND, MyDatabaseId, relid,
										  NULL);
	return (IHeapStatCounts *) entry_ref->pending;
}

static bool
iheap_stat_flush_pending(PgStat_EntryRef *entry_ref, bool nowait)
{
	IHeapStatCounts *p = (IHeapStatCounts *) entry_ref->pending;
	IHeapStatCounts *s = &((PgStatShared_IHeap *) entry_ref->shared_stats)->stats;

	if (!pgstat_lock_entry(entry_ref, nowait))
		return false;

	s->inplace_updates += p->inplace_updates;
	s->inplace_key_updates += p->inplace_key_updates;
	for (int i = 0; i < IHEAP_MOVE_NREASONS; i++)
		s->moved_updates[i] += p->moved_updates[i];
	s->undo_records += p->undo_records;
	s->undo_bytes += p->undo_bytes;
	for (int i = 0; i < IHEAP_ROLLBACK_NKINDS; i++)
		s->rollback_records[i] += p->rollback_records[i];
	s->rollback_bytes += p->rollback_bytes;
	s->discarded_pages += p->discarded_pages;
	s->discarded_bytes += p->discarded_bytes;
	s->walks += p->walks;
	s->walk_steps += p->walk_steps;
	s->walk_max_steps = Max(s->walk_max_steps, p->walk_max_steps);

	pgstat_unlock_entry(entry_ref);
	return true;
}

static void
iheap_stat_reset_timestamp(PgStatShared_Common *header, TimestampTz ts)
{
	((PgStatShared_IHeap *) header)->stats.stat_reset_timestamp = ts;
}

/* ------------------------------------------------------------------------
 * Counting
 * ------------------------------------------------------------------------
 */

/*
 * An update of rel: kept in place (IHEAP_MOVE_NONE; keys: it changed key
 * columns of dmbtree indexes), or moved for the given reason.
 */
void
iheap_stat_count_update(Relation rel, IHeapMoveReason reason, bool keys)
{
	IHeapStatCounts *c = pending_counts(RelationGetRelid(rel));

	if (c == NULL)
		return;
	if (reason == IHEAP_MOVE_NONE)
	{
		c->inplace_updates++;
		if (keys)
			c->inplace_key_updates++;
	}
	else
		c->moved_updates[reason]++;
}

/* undo records written to rel */
void
iheap_stat_count_undo(Relation rel, int nrecords, uint64 nbytes)
{
	IHeapStatCounts *c = pending_counts(RelationGetRelid(rel));

	if (c == NULL)
		return;
	c->undo_records += nrecords;
	c->undo_bytes += nbytes;
}

/* a reader rebuilt "steps" older versions of a row of rel from undo */
void
iheap_stat_count_walk(Relation rel, int steps)
{
	IHeapStatCounts *c = pending_counts(RelationGetRelid(rel));

	if (c == NULL)
		return;
	c->walks++;
	c->walk_steps += steps;
	c->walk_max_steps = Max(c->walk_max_steps, steps);
}

/* discard passed "pages" undo pages of rel, holding "bytes" of records */
void
iheap_stat_count_discard(Relation rel, int64 pages, int64 bytes)
{
	IHeapStatCounts *c;

	if (pages == 0)
		return;
	c = pending_counts(RelationGetRelid(rel));
	if (c == NULL)
		return;
	c->discarded_pages += pages;
	c->discarded_bytes += bytes;
}

/*
 * Rollback applied an undo record of "len" bytes.  Called inside critical
 * sections, so only tallied; the caller of the rollback reports the tally.
 */
void
iheap_stat_rollback_applied(uint32 len)
{
	rollback_tally_records++;
	rollback_tally_bytes += len;
}

/* count the tally for relation relid, as rollback of the given kind */
void
iheap_stat_rollback_report(Oid relid, IHeapRollbackKind kind)
{
	IHeapStatCounts *c;

	if (rollback_tally_records == 0)
		return;
	c = pending_counts(relid);
	if (c != NULL)
	{
		c->rollback_records[kind] += rollback_tally_records;
		c->rollback_bytes += rollback_tally_bytes;
	}
	rollback_tally_records = rollback_tally_bytes = 0;
}

/* forget a tally an error left behind */
void
iheap_stat_rollback_forget(void)
{
	rollback_tally_records = rollback_tally_bytes = 0;
}

/*
 * A relation created by the current subtransaction: its entry goes if the
 * creation rolls back (and an entry left by an earlier relation with the
 * same OID is reset).
 */
void
iheap_stat_relation_created(Oid relid)
{
	if (OidIsValid(MyDatabaseId))
		pgstat_create_transactional(IHEAP_STAT_KIND, MyDatabaseId, relid);
}

/* relation relid is being dropped: its entry goes when the drop commits */
void
iheap_stat_relation_dropped(Oid relid)
{
	if (OidIsValid(MyDatabaseId) &&
		pgstat_have_entry(IHEAP_STAT_KIND, MyDatabaseId, relid))
		pgstat_drop_transactional(IHEAP_STAT_KIND, MyDatabaseId, relid);
}

/* ------------------------------------------------------------------------
 * SQL interface
 * ------------------------------------------------------------------------
 */

PG_FUNCTION_INFO_V1(iheap_stat);
PG_FUNCTION_INFO_V1(iheap_stat_reset);
PG_FUNCTION_INFO_V1(iheap_stat_reset_all);

#define IHEAP_STAT_COLS		19

/*
 * iheap_stat(rel) -> (inplace_updates, inplace_key_updates, moved_updates,
 *		moved_for_index, moved_for_trigger, moved_for_size, moved_for_setting,
 *		undo_records, undo_bytes, rollback_eager, rollback_lazy,
 *		rollback_discard, rollback_bytes, discarded_pages, discarded_bytes,
 *		undo_walks, undo_walk_steps, undo_walk_max, stats_reset)
 *
 * Zeros for a relation without an entry.  Like core's statistics functions,
 * it takes no lock and reads what the stats snapshot holds.
 */
Datum
iheap_stat(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	IHeapStatCounts *c;
	IHeapStatCounts zero;
	TupleDesc	tupdesc;
	Datum		values[IHEAP_STAT_COLS];
	bool		nulls[IHEAP_STAT_COLS] = {0};
	int			i = 0;
	PgStat_Counter moved = 0;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	c = (IHeapStatCounts *) pgstat_fetch_entry(IHEAP_STAT_KIND, MyDatabaseId,
											   relid, NULL);
	if (c == NULL)
	{
		memset(&zero, 0, sizeof(zero));
		c = &zero;
	}
	for (int r = 0; r < IHEAP_MOVE_NREASONS; r++)
		moved += c->moved_updates[r];

	values[i++] = Int64GetDatum(c->inplace_updates);
	values[i++] = Int64GetDatum(c->inplace_key_updates);
	values[i++] = Int64GetDatum(moved);
	values[i++] = Int64GetDatum(c->moved_updates[IHEAP_MOVE_INDEX]);
	values[i++] = Int64GetDatum(c->moved_updates[IHEAP_MOVE_TRIGGER]);
	values[i++] = Int64GetDatum(c->moved_updates[IHEAP_MOVE_SIZE]);
	values[i++] = Int64GetDatum(c->moved_updates[IHEAP_MOVE_SETTING]);
	values[i++] = Int64GetDatum(c->undo_records);
	values[i++] = Int64GetDatum(c->undo_bytes);
	values[i++] = Int64GetDatum(c->rollback_records[IHEAP_ROLLBACK_EAGER]);
	values[i++] = Int64GetDatum(c->rollback_records[IHEAP_ROLLBACK_LAZY]);
	values[i++] = Int64GetDatum(c->rollback_records[IHEAP_ROLLBACK_DISCARD]);
	values[i++] = Int64GetDatum(c->rollback_bytes);
	values[i++] = Int64GetDatum(c->discarded_pages);
	values[i++] = Int64GetDatum(c->discarded_bytes);
	values[i++] = Int64GetDatum(c->walks);
	values[i++] = Int64GetDatum(c->walk_steps);
	values[i++] = Int64GetDatum(c->walk_max_steps);
	if (c->stat_reset_timestamp != 0)
		values[i] = TimestampTzGetDatum(c->stat_reset_timestamp);
	else
		nulls[i] = true;
	i++;
	Assert(i == IHEAP_STAT_COLS);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* iheap_stat_reset(rel): reset the counters of one iheap relation */
Datum
iheap_stat_reset(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);

	/* resetting would create an entry: only for iheap relations */
	relation_close(iheap_open_relation(relid, AccessShareLock),
				   AccessShareLock);

	pgstat_reset(IHEAP_STAT_KIND, MyDatabaseId, relid);
	PG_RETURN_VOID();
}

/* iheap_stat_reset(): reset the counters of every iheap relation */
Datum
iheap_stat_reset_all(PG_FUNCTION_ARGS)
{
	pgstat_reset_of_kind(IHEAP_STAT_KIND);
	PG_RETURN_VOID();
}
