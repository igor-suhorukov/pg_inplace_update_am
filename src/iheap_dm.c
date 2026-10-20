/*-------------------------------------------------------------------------
 *
 * iheap_dm.c
 *	  iheap's side of dmbtree: in-place updates of indexed columns
 *	  (DESIGN.md §7.4, §11.5, §11.6, §21 D22-D27, D39).
 *
 * An UPDATE that changes a column a dmbtree index reads keeps the row at
 * its TID.  iheap then maintains such indexes itself: it inserts an entry
 * for the new key and delete-marks the entry for the old one (the executor
 * can only add entries to every index, TU_All, or to none).  The new entry
 * is marked too, since the update may still abort and rollback never
 * touches indexes: a marked entry may disagree with its row, so whoever
 * reaches a row through one checks the row's key against it.  An unmarked
 * entry agrees with the newest committed version of its row.  Expressions
 * and predicates are evaluated wherever a row's entry is computed, and an
 * update that changes only INCLUDE values marks the entry, which keeps its
 * values: index-only scans take them from the row for a marked entry.
 *
 * This file has:
 *	- the per-relation cache of the indexes that can take in-place updates
 *	  of the columns they read, and of the columns whose change moves the
 *	  row;
 *	- the evaluation of index expressions and predicates on row versions;
 *	- the index maintenance after an in-place update;
 *	- the key check (iheap_dm_matches), used by index fetches (iheap_scan.c)
 *	  and by the functions dmbtree calls (dmbtree_api.h): the check for
 *	  index-only scans, the probe for uniqueness checks, the fate of marked
 *	  entries for cleanup, and the check of entries against their rows for
 *	  dmbtree_check().
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/relation.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_index.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "nodes/bitmapset.h"
#include "optimizer/optimizer.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "utils/datum.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "iheap.h"
#include "dmbtree_api.h"

/* ------------------------------------------------------------------------
 * Which indexes can take in-place updates of the columns they read
 * ------------------------------------------------------------------------
 */
static HTAB *dm_infos = NULL;
static HTAB *dm_owners = NULL;	/* index OID -> table, for the dm[] indexes */
static uint64 dm_invalidations = 0;	/* callback calls, to detect races */
static int	dm_nstale = 0;		/* entries gone stale since the last sweep */

typedef struct DmOwner
{
	Oid			indexoid;		/* hash key */
	Oid			relid;
} DmOwner;

static void dm_eval_forget(void);

static void
dm_mark_stale(IHeapDmInfo *info)
{
	info->valid = false;
	dm_nstale++;
}

/*
 * A relcache invalidation of the table (CREATE/DROP INDEX, changes of an
 * index's validity, REINDEX) or of one of its indexes.  It can arrive
 * whenever a lock is taken, also while an update holds on to the entry, so
 * the entry is only marked stale here; the next iheap_dm_info() call
 * rebuilds it, and the end of the transaction frees it if none did (the
 * table may be gone).
 */
static void
dm_relcache_callback(Datum arg, Oid relid)
{
	IHeapDmInfo *info;
	DmOwner    *owner;

	dm_invalidations++;
	if (dm_infos == NULL)
		return;

	if (!OidIsValid(relid))
	{
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, dm_infos);
		while ((info = hash_seq_search(&status)) != NULL)
			dm_mark_stale(info);
		return;
	}

	info = hash_search(dm_infos, &relid, HASH_FIND, NULL);
	if (info != NULL && info->valid)
		dm_mark_stale(info);
	owner = hash_search(dm_owners, &relid, HASH_FIND, NULL);
	if (owner != NULL)
	{
		info = hash_search(dm_infos, &owner->relid, HASH_FIND, NULL);
		if (info != NULL && info->valid)
			dm_mark_stale(info);
	}
}

/* forget info's contents, and the owner entries of its indexes */
static void
dm_free_contents(IHeapDmInfo *info)
{
	if (info->cxt == NULL)
		return;
	for (int i = 0; i < info->ndm; i++)
	{
		DmOwner    *owner = hash_search(dm_owners, &info->dm[i].indexoid,
										HASH_FIND, NULL);

		if (owner != NULL && owner->relid == info->relid)
			hash_search(dm_owners, &info->dm[i].indexoid, HASH_REMOVE, NULL);
	}
	MemoryContextDelete(info->cxt);
	info->cxt = NULL;
	info->ndm = 0;
}

/*
 * At the end of a transaction nobody holds an entry: free the stale ones,
 * and the evaluation states of index expressions.
 */
static void
dm_xact_callback(XactEvent event, void *arg)
{
	HASH_SEQ_STATUS status;
	IHeapDmInfo *info;

	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT &&
		event != XACT_EVENT_PREPARE && event != XACT_EVENT_PARALLEL_COMMIT &&
		event != XACT_EVENT_PARALLEL_ABORT)
		return;

	dm_eval_forget();

	if (dm_nstale == 0)
		return;
	hash_seq_init(&status, dm_infos);
	while ((info = hash_seq_search(&status)) != NULL)
	{
		if (info->valid)
			continue;
		dm_free_contents(info);
		hash_search(dm_infos, &info->relid, HASH_REMOVE, NULL);
	}
	dm_nstale = 0;
}

void
iheap_dm_init(void)
{
	CacheRegisterRelcacheCallback(dm_relcache_callback, (Datum) 0);
	RegisterXactCallback(dm_xact_callback, NULL);
}

static void
add_attnum(Bitmapset **set, AttrNumber attnum)
{
	*set = bms_add_member(*set, attnum - FirstLowInvalidHeapAttributeNumber);
}

/* does "index" have expressions or a predicate? */
static bool
dm_index_computed(Relation index)
{
	return !heap_attisnull(index->rd_indextuple, Anum_pg_index_indexprs, NULL) ||
		!heap_attisnull(index->rd_indextuple, Anum_pg_index_indpred, NULL);
}

/*
 * The cached facts about rel's indexes.  An index takes in-place updates of
 * the columns it reads when it is a dmbtree index that can carry
 * delete-marks (dmbtree_dm_capable), is valid and ready, and its uniqueness
 * is checked immediately: a deferred check or an exclusion constraint is
 * the executor's business, which needs entries for a new TID (rule 2 of
 * §7.4).  Every column of any other index (summarizing ones aside, which
 * TU_Summarizing handles), and every column its expressions and predicate
 * read, forces a move.
 *
 * The result stays valid until the next call for the same relation.
 */
IHeapDmInfo *
iheap_dm_info(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	IHeapDmInfo *info;
	bool		found;
	List	   *indexoids;
	ListCell   *lc;
	MemoryContext oldcxt;
	int			maxdm;
	uint64		invalidations;

	if (dm_infos == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(IHeapDmInfo);
		ctl.hcxt = CacheMemoryContext;
		dm_infos = hash_create("iheap dmbtree indexes", 64, &ctl,
							   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		ctl.entrysize = sizeof(DmOwner);
		dm_owners = hash_create("iheap dmbtree index owners", 64, &ctl,
								HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	info = hash_search(dm_infos, &relid, HASH_ENTER, &found);
	if (!found)
	{
		info->cxt = NULL;
		info->ndm = 0;
		dm_mark_stale(info);	/* swept if never built */
	}
	if (info->valid)
		return info;

restart:
	dm_free_contents(info);
	invalidations = dm_invalidations;

	indexoids = RelationGetIndexList(rel);

	info->cxt = AllocSetContextCreate(CacheMemoryContext, "iheap dmbtree index info",
									  ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(info->cxt);
	info->blocking = NULL;
	info->dmattrs = NULL;
	info->ndm = 0;
	maxdm = Max(list_length(indexoids), 1);
	info->dm = palloc0(maxdm * sizeof(IHeapDmIndex));
	MemoryContextSwitchTo(oldcxt);

	foreach(lc, indexoids)
	{
		Relation	irel = index_open(lfirst_oid(lc), AccessShareLock);
		Form_pg_index idx = irel->rd_index;
		int			natts = IndexRelationGetNumberOfAttributes(irel);
		int			nkeyatts = IndexRelationGetNumberOfKeyAttributes(irel);
		Bitmapset  *attrs = NULL;

		if (irel->rd_indam->amsummarizing)
		{
			index_close(irel, AccessShareLock);
			continue;
		}

		oldcxt = MemoryContextSwitchTo(info->cxt);
		if (idx->indisvalid && idx->indisready && idx->indislive &&
			!idx->indisexclusion &&
			(!idx->indisunique || idx->indimmediate) &&
			dmbtree_dm_capable(irel))
		{
			IHeapDmIndex *d = &info->dm[info->ndm++];

			d->indexoid = RelationGetRelid(irel);
			d->natts = natts;
			d->nkeyatts = nkeyatts;
			d->unique = idx->indisunique;
			d->computed = dm_index_computed(irel);
			d->keyattrs = NULL;
			d->incattrs = NULL;
			for (int i = 0; i < natts; i++)
			{
				d->attnums[i] = idx->indkey.values[i];
				if (d->attnums[i] == 0)
					continue;	/* an expression: its columns below */
				if (i < nkeyatts)
					add_attnum(&d->keyattrs, d->attnums[i]);
				else
					add_attnum(&d->incattrs, d->attnums[i]);
			}
			if (d->computed)
			{
				pull_varattnos((Node *) RelationGetIndexExpressions(irel), 1,
							   &d->keyattrs);
				pull_varattnos((Node *) RelationGetIndexPredicate(irel), 1,
							   &d->keyattrs);
			}
			info->dmattrs = bms_add_members(info->dmattrs, d->keyattrs);
			info->dmattrs = bms_add_members(info->dmattrs, d->incattrs);
		}
		else
		{
			for (int i = 0; i < natts; i++)
			{
				if (idx->indkey.values[i] != 0)
					add_attnum(&attrs, idx->indkey.values[i]);
			}
			pull_varattnos((Node *) RelationGetIndexExpressions(irel), 1, &attrs);
			pull_varattnos((Node *) RelationGetIndexPredicate(irel), 1, &attrs);
			info->blocking = bms_add_members(info->blocking, attrs);
		}
		MemoryContextSwitchTo(oldcxt);

		index_close(irel, AccessShareLock);
	}
	list_free(indexoids);

	/* opening the indexes may have processed invalidations: be sure */
	if (dm_invalidations != invalidations)
		goto restart;

	for (int i = 0; i < info->ndm; i++)
	{
		DmOwner    *owner = hash_search(dm_owners, &info->dm[i].indexoid,
										HASH_ENTER, NULL);

		owner->relid = relid;
	}
	info->valid = true;
	return info;
}

/* ------------------------------------------------------------------------
 * Evaluating index expressions and predicates (§21 D39)
 *
 * Computing the entry of a row version for an index with expressions or a
 * predicate takes executor state: built once per transaction and index,
 * freed with all others at the end of the transaction (never while in use:
 * a nested evaluation for the same index, an index expression whose
 * function reads the table, gets a state of its own).  Callers evaluate
 * with no buffer locked: the functions are the user's, and may take locks
 * and read.
 * ------------------------------------------------------------------------
 */
typedef struct DmEvalState
{
	Oid			indexoid;		/* hash key */
	IndexInfo  *ii;
	EState	   *estate;
	TupleTableSlot *slot;		/* virtual, with a copy of the table's
								 * descriptor */
	ExprState  *pred;			/* NULL if no predicate */
	bool		in_use;
} DmEvalState;

static MemoryContext dm_eval_cxt = NULL;	/* everything of the transaction */
static HTAB *dm_evals = NULL;

static void
dm_eval_forget(void)
{
	if (dm_evals == NULL)
		return;
	dm_evals = NULL;
	MemoryContextReset(dm_eval_cxt);
}

static void
dm_eval_build(DmEvalState *ev, Relation index, Relation heaprel)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(dm_eval_cxt);

	ev->ii = BuildIndexInfo(index);
	ev->estate = CreateExecutorState();
	ev->slot = MakeSingleTupleTableSlot(CreateTupleDescCopy(RelationGetDescr(heaprel)),
										&TTSOpsVirtual);
	GetPerTupleExprContext(ev->estate)->ecxt_scantuple = ev->slot;
	ev->pred = (ev->ii->ii_Predicate != NIL) ?
		ExecPrepareQual(ev->ii->ii_Predicate, ev->estate) : NULL;
	ev->in_use = false;
	MemoryContextSwitchTo(oldcxt);
}

static DmEvalState *
dm_eval_get(Relation index, Relation heaprel)
{
	Oid			indexoid = RelationGetRelid(index);
	DmEvalState *ev;
	bool		found;

	if (dm_eval_cxt == NULL)
		dm_eval_cxt = AllocSetContextCreate(TopMemoryContext,
											"iheap dmbtree index evaluation",
											ALLOCSET_DEFAULT_SIZES);
	if (dm_evals == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(DmEvalState);
		ctl.hcxt = dm_eval_cxt;
		dm_evals = hash_create("iheap dmbtree index evaluation", 16, &ctl,
							   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	ev = hash_search(dm_evals, &indexoid, HASH_ENTER, &found);
	if (found && ev->in_use)
	{
		/* in use further up the stack: one of its own */
		ev = MemoryContextAllocZero(dm_eval_cxt, sizeof(DmEvalState));
		ev->indexoid = indexoid;
		dm_eval_build(ev, index, heaprel);
	}
	else if (!found ||
			 ev->slot->tts_tupleDescriptor->natts != RelationGetDescr(heaprel)->natts)
	{
		/* (the table's columns changed: the old state stays till the end) */
		dm_eval_build(ev, index, heaprel);
	}
	ev->in_use = true;
	return ev;
}

static void
dm_eval_release(DmEvalState *ev)
{
	ExecClearTuple(ev->slot);
	ResetExprContext(GetPerTupleExprContext(ev->estate));
	ev->in_use = false;
}

/*
 * The entry values of a row version, from its column values (tvalues,
 * tisnull, by the table's descriptor), copied into the current memory
 * context; false if the predicate leaves the version out of the index.
 */
static bool
dm_eval_entry(DmEvalState *ev, Relation index, const Datum *tvalues,
			  const bool *tisnull, Datum *ivalues, bool *iisnull)
{
	TupleTableSlot *slot = ev->slot;
	ExprContext *econtext = GetPerTupleExprContext(ev->estate);
	TupleDesc	idesc = RelationGetDescr(index);
	int			natts = slot->tts_tupleDescriptor->natts;
	bool		in;

	ExecClearTuple(slot);
	memcpy(slot->tts_values, tvalues, natts * sizeof(Datum));
	memcpy(slot->tts_isnull, tisnull, natts * sizeof(bool));
	ExecStoreVirtualTuple(slot);

	in = (ev->pred == NULL || ExecQual(ev->pred, econtext));
	if (in)
	{
		FormIndexDatum(ev->ii, slot, ev->estate, ivalues, iisnull);
		for (int i = 0; i < ev->ii->ii_NumIndexAttrs; i++)
		{
			CompactAttribute *att = TupleDescCompactAttr(idesc, i);

			if (!iisnull[i])
				ivalues[i] = datumCopy(ivalues[i], att->attbyval, att->attlen);
		}
	}
	ExecClearTuple(slot);
	ResetExprContext(econtext);
	return in;
}

/*
 * The values of the entry index has for row version "tuple" (key and
 * INCLUDE columns), or false if the index's predicate leaves the version
 * out.  The caller locks no buffer (expressions are evaluated); values of
 * an expression are palloc'd, those of plain columns point into the tuple.
 */
bool
iheap_dm_entry_values(Relation rel, Relation index, const char *tuple,
					  Datum *ivalues, bool *iisnull)
{
	TupleDesc	tdesc = RelationGetDescr(rel);
	int			natts = IndexRelationGetNumberOfAttributes(index);
	Datum	   *values = palloc(Max(tdesc->natts, 1) * sizeof(Datum));
	bool	   *isnull = palloc(Max(tdesc->natts, 1) * sizeof(bool));
	bool		in = true;

	ih_deform_tuple(tdesc, tuple, values, isnull);
	if (dm_index_computed(index))
	{
		DmEvalState *ev = dm_eval_get(index, rel);

		in = dm_eval_entry(ev, index, values, isnull, ivalues, iisnull);
		dm_eval_release(ev);
	}
	else
	{
		for (int i = 0; i < natts; i++)
		{
			int			col = index->rd_index->indkey.values[i] - 1;

			ivalues[i] = values[col];
			iisnull[i] = isnull[col];
		}
	}
	pfree(values);
	pfree(isnull);
	return in;
}

/* are entry values a and b of index columns [from, to) the same, by image? */
static bool
dm_values_equal(Relation index, int from, int to, const Datum *a,
				const bool *anull, const Datum *b, const bool *bnull)
{
	TupleDesc	idesc = RelationGetDescr(index);

	for (int i = from; i < to; i++)
	{
		CompactAttribute *att = TupleDescCompactAttr(idesc, i);

		if (anull[i] != bnull[i])
			return false;
		if (!anull[i] && !datum_image_eq(a[i], b[i], att->attbyval, att->attlen))
			return false;
	}
	return true;
}

/* ------------------------------------------------------------------------
 * Index maintenance after an in-place update
 * ------------------------------------------------------------------------
 */

/*
 * The in-place update of row tid changed the columns "modified": for every
 * index whose entry it changed, insert the entry for the new key and
 * delete-mark the entry for the old one (§11.6).  The table change is done
 * and its page lock released: index pages are never locked while a table
 * page is (nbtree's uniqueness checks lock them the other way round), and
 * expressions are evaluated.  Other writers wait on our row until we
 * finish; readers of our transaction see the row only after this command,
 * when both entries are there.
 *
 * The new entry goes first: if it violates uniqueness, the transaction
 * aborts before the old one is marked (either order is safe).  With a
 * predicate, a version that enters the index gets an entry and one that
 * leaves it has its entry marked.  A change of INCLUDE values only marks
 * the entry, which keeps its values (§21 D39): an entry stands for every
 * version of its row with its key, and (key, TID) has room for one.
 * Returns whether any entry was added or marked.
 */
bool
iheap_dm_update_indexes(Relation rel, ItemPointer tid,
						const Bitmapset *modified,
						const Datum *oldvalues, const bool *oldisnull,
						const Datum *newvalues, const bool *newisnull)
{
	IHeapDmInfo *info = iheap_dm_info(rel);
	MemoryContext cxt;
	MemoryContext oldcxt;
	bool		changed = false;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "iheap dmbtree update",
								ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	for (int i = 0; i < info->ndm; i++)
	{
		IHeapDmIndex *d = &info->dm[i];
		Relation	irel;
		Datum		oldkey[INDEX_MAX_KEYS];
		bool		oldnull[INDEX_MAX_KEYS];
		Datum		newkey[INDEX_MAX_KEYS];
		bool		newnull[INDEX_MAX_KEYS];
		bool		old_in = true;
		bool		new_in = true;

		if (!bms_overlap(d->keyattrs, modified) &&
			!bms_overlap(d->incattrs, modified))
			continue;

		irel = index_open(d->indexoid, RowExclusiveLock);
		if (d->computed)
		{
			DmEvalState *ev = dm_eval_get(irel, rel);

			old_in = dm_eval_entry(ev, irel, oldvalues, oldisnull, oldkey, oldnull);
			new_in = dm_eval_entry(ev, irel, newvalues, newisnull, newkey, newnull);
			dm_eval_release(ev);
		}
		else
		{
			for (int a = 0; a < d->natts; a++)
			{
				int			col = d->attnums[a] - 1;

				oldkey[a] = oldvalues[col];
				oldnull[a] = oldisnull[col];
				newkey[a] = newvalues[col];
				newnull[a] = newisnull[col];
			}
		}

		if (old_in && new_in)
		{
			if (!dm_values_equal(irel, 0, d->nkeyatts, oldkey, oldnull,
								 newkey, newnull))
			{
				dmbtree_inplace_insert(irel, rel, newkey, newnull, tid, d->unique);
				dmbtree_inplace_mark(irel, rel, oldkey, oldnull, tid);
				changed = true;
			}
			else if (!dm_values_equal(irel, d->nkeyatts, d->natts, oldkey,
									  oldnull, newkey, newnull))
			{
				dmbtree_check_entry_size(irel, rel, newkey, newnull, tid);
				dmbtree_inplace_mark(irel, rel, oldkey, oldnull, tid);
				changed = true;
			}
		}
		else if (new_in)
		{
			dmbtree_inplace_insert(irel, rel, newkey, newnull, tid, d->unique);
			changed = true;
		}
		else if (old_in)
		{
			dmbtree_inplace_mark(irel, rel, oldkey, oldnull, tid);
			changed = true;
		}
		index_close(irel, NoLock);
		MemoryContextReset(cxt);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
	return changed;
}

/* ------------------------------------------------------------------------
 * The key check
 * ------------------------------------------------------------------------
 */

/*
 * Does the row version "tuple" have the entry dm: the key of dm->itup, and
 * with IHEAP_DM_INCLUDE its INCLUDE values too?  A version the index's
 * predicate leaves out has no entry.  Returns 1 or 0, or -1 if it cannot
 * tell without what "flags" does not allow: evaluating an index expression
 * or predicate (IHEAP_DM_EVAL), fetching a value stored out of line
 * (IHEAP_DM_FETCH; for an index with expressions, any value of the version
 * stored out of line counts, as the expressions may read it).
 *
 * The index's opclasses have "equality implies image equality"
 * (dmbtree_dm_capable), so keys are compared by image, which also copes
 * with values compressed differently in the index and the table.  INCLUDE
 * values are compared by image too: that is what an index-only scan
 * returns.
 */
int
iheap_dm_matches(Relation rel, const IHeapDmEntry *dm, const char *tuple,
				 int flags)
{
	Relation	index = dm->index;
	TupleDesc	idesc = RelationGetDescr(index);
	int			ncmp = (flags & IHEAP_DM_INCLUDE) ?
		IndexRelationGetNumberOfAttributes(index) :
		IndexRelationGetNumberOfKeyAttributes(index);
	bool		computed = dm_index_computed(index);
	Datum		ivalues[INDEX_MAX_KEYS];
	bool		iisnull[INDEX_MAX_KEYS];
	int			result = 1;

	if (computed &&
		(!(flags & IHEAP_DM_EVAL) ||
		 (!(flags & IHEAP_DM_FETCH) &&
		  (ih_hdr_infomask(tuple) & IHT_HASEXTERNAL))))
		return -1;

	if (!iheap_dm_entry_values(rel, index, tuple, ivalues, iisnull))
		return 0;

	for (int i = 0; i < ncmp && result == 1; i++)
	{
		CompactAttribute *att = TupleDescCompactAttr(idesc, i);
		bool		inull;
		Datum		ival = index_getattr(dm->itup, i + 1, idesc, &inull);

		if (inull != iisnull[i])
			result = 0;
		else if (inull)
			continue;
		else if (!(flags & IHEAP_DM_FETCH) && att->attlen == -1 &&
				 VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(ivalues[i])))
			result = -1;
		else if (!datum_image_eq(ival, ivalues[i], att->attbyval, att->attlen))
			result = 0;
	}

	/* values computed by expressions were copied for us */
	if (computed)
	{
		for (int i = 0; i < IndexRelationGetNumberOfAttributes(index); i++)
		{
			if (!iisnull[i] && !TupleDescCompactAttr(idesc, i)->attbyval)
				pfree(DatumGetPointer(ivalues[i]));
		}
	}
	return result;
}

/* ------------------------------------------------------------------------
 * For dmbtree (dmbtree_api.h)
 * ------------------------------------------------------------------------
 */

void
iheap_index_fetch_set_entry(IndexFetchTableData *fetch, Relation index,
							IndexTuple itup, bool marked)
{
	IHeapIndexFetchData *f = (IHeapIndexFetchData *) fetch;

	f->xs_dm.index = index;
	f->xs_dm.itup = itup;
	f->xs_dm.marked = marked;
	f->xs_dm.locked = false;
}

/*
 * Index-only scans: does row tid have a version visible to snapshot (an
 * MVCC snapshot) whose key is that of the marked entry itup?  The scan
 * passes over the entry if not, without the executor visiting the row, so
 * a serializable scan records here the rw-conflicts with the writers of
 * versions it does not see, as the fetch of the row would.  If the version
 * has other INCLUDE values than the entry (an in-place update changed them,
 * §21 D39), *subst gets an entry with the version's values, which the scan
 * returns instead.
 */
bool
iheap_dm_entry_visible(Relation heaprel, ItemPointer tid, Snapshot snapshot,
					   Relation index, IndexTuple itup, IndexTuple *subst)
{
	BlockNumber blk = ItemPointerGetBlockNumber(tid);
	OffsetNumber off = ItemPointerGetOffsetNumber(tid);
	IHeapDmEntry dm = {index, itup, true, false};
	bool		ssi = CheckForSerializableConflictOutNeeded(heaprel, snapshot);
	bool		include = subst != NULL &&
		IndexRelationGetNumberOfAttributes(index) >
		IndexRelationGetNumberOfKeyAttributes(index);
	Buffer		buf;
	Page		page;
	ItemId		lp;
	IHeapVersion v;
	int			match = 0;

	if (subst)
		*subst = NULL;
	if (blk >= RelationGetNumberOfBlocks(heaprel))
		return false;
	buf = ReadBufferExtended(heaprel, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	if (IHeapPageIsData(page) && off >= FirstOffsetNumber &&
		off <= PageGetMaxOffsetNumber(page) &&
		ItemIdIsNormal(lp = PageGetItemId(page, off)) &&
		iheap_resolve_version_ext(heaprel, buf, (char *) page + ItemIdGetOffset(lp),
								  ItemIdGetLength(lp), NULL, snapshot, &v,
								  false, ssi))
	{
		match = include ? -1 : iheap_dm_matches(heaprel, &dm, v.tuple, 0);
		/* expressions and values out of line: with the page unlocked */
		if (match < 0 && v.mem == NULL)
			v.tuple = ih_copy_tuple_aligned(CurrentMemoryContext, v.tuple,
											v.len, &v.mem);
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		if (match < 0)
			match = iheap_dm_matches(heaprel, &dm, v.tuple, IHEAP_DM_UNLOCKED);
		if (match == 1 && include &&
			iheap_dm_matches(heaprel, &dm, v.tuple,
							 IHEAP_DM_UNLOCKED | IHEAP_DM_INCLUDE) != 1)
		{
			Datum		ivalues[INDEX_MAX_KEYS];
			bool		iisnull[INDEX_MAX_KEYS];

			if (iheap_dm_entry_values(heaprel, index, v.tuple, ivalues, iisnull))
			{
				*subst = index_form_tuple(RelationGetDescr(index), ivalues,
										  iisnull);
				(*subst)->t_tid = *tid;
			}
		}
		if (v.mem)
			pfree(v.mem);
	}
	else
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buf);
	return match == 1;
}

/*
 * table_index_fetch_tuple_check() for an entry of a dmbtree index, for
 * uniqueness checks (iheap_index_fetch_tuple does the key check).
 */
bool
iheap_dm_fetch_check(Relation heaprel, ItemPointer tid, Snapshot snapshot,
					 Relation index, IndexTuple itup, bool marked,
					 bool *all_dead)
{
	IndexFetchTableData *scan;
	TupleTableSlot *slot;
	bool		call_again = false;
	bool		found;

	slot = table_slot_create(heaprel, NULL);
	scan = table_index_fetch_begin(heaprel, SO_NONE);
	iheap_index_fetch_set_entry(scan, index, itup, marked);
	/* dmbtree's uniqueness check holds the page: the mark is current */
	((IHeapIndexFetchData *) scan)->xs_dm.locked = true;
	found = table_index_fetch_tuple(scan, tid, snapshot, slot, &call_again,
									all_dead);
	table_index_fetch_end(scan);
	ExecDropSingleTupleTableSlot(slot);

	return found;
}

static int
dm_check_cmp(const void *a, const void *b)
{
	const DmbtEntryCheck *ea = (const DmbtEntryCheck *) a;
	const DmbtEntryCheck *eb = (const DmbtEntryCheck *) b;

	return ItemPointerCompare(&ea->tid, &eb->tid);
}

/* a version of a row on the walk of dm_entry_status() */
typedef struct DmWalkStep
{
	char	   *tuple;			/* a copy (NULL for a deletion or move) */
	bool		frozen;			/* frozen: visible to all, nothing older is */
	IHeapXidStatus st;
	TransactionId xid;
	bool		removable;		/* committed, and every snapshot sees it */
	bool		onpage;			/* the version on the page */
} DmWalkStep;

/*
 * The versions of the row at off of the locked page buf that dm_entry_status()
 * decides by, newest first, copied: from the version on the page down to
 * the first one every snapshot sees (frozen, or committed and removable),
 * or to the version that inserted the row.  Returns how many; -1 when the
 * row is gone (nothing to keep the entry for), -2 when the walk cannot be
 * finished (KEEP).  *ended tells that the walk reached the insert: no
 * version before the last one returned ever existed.
 */
#define DM_WALK_MAX 100000

static int
dm_walk(Relation rel, Buffer buf, OffsetNumber off, GlobalVisState *vistest,
		DmWalkStep **stepsp, bool *ended)
{
	Page		page = BufferGetPage(buf);
	UndoRecPtr	discard = iheap_discard_ptr(rel);
	ItemId		lp;
	IHeapHdr	h;
	char	   *cur;
	uint32		curlen;
	bool		onpage = true;
	int			n = 0;
	int			cap = 4;
	DmWalkStep *steps;
	int			nsteps = 0;

	*ended = false;
	if (!IHeapPageIsData(page) || off < FirstOffsetNumber ||
		off > PageGetMaxOffsetNumber(page))
		return -1;
	lp = PageGetItemId(page, off);
	if (!ItemIdIsNormal(lp))
		return -1;				/* dead or unused: the row is gone */
	cur = (char *) page + ItemIdGetOffset(lp);
	curlen = ItemIdGetLength(lp);
	ih_hdr_read(cur, &h);

	steps = palloc(cap * sizeof(DmWalkStep));
	for (;;)
	{
		bool		deleted = (h.infomask & (IHT_DELETED | IHT_MOVED)) != 0;
		IHeapUndoRec *rec;
		DmWalkStep *s;
		char	   *mem;

		if (++nsteps > DM_WALK_MAX)
			return -2;

		/* a LOCK record in front of the version is transparent */
		if (h.urec >= discard && (h.infomask & IHT_LOCKED))
		{
			rec = iheap_undo_fetch(rel, h.urec, true);
			if (rec == NULL)
			{
				/* discarded under us: committed, visible to all */
				h.urec = InvalidUndoRecPtr;
			}
			else
			{
				if (rec->hdr.ur_type == IHU_LOCK)
				{
					IHeapHdr	ph;

					ih_hdr_read(rec->hdr.ur_prev_hdr, &ph);
					h.infomask = ph.infomask | (h.infomask & (IHT_XID_HINTS | IHT_STUB));
					h.urec = ph.urec;
					h.xid = ph.xid;
					pfree(rec);
					continue;
				}
				pfree(rec);
			}
		}

		if (n == cap)
		{
			cap *= 2;
			steps = repalloc(steps, cap * sizeof(DmWalkStep));
		}
		s = &steps[n++];
		s->tuple = deleted ? NULL :
			(onpage ? ih_copy_tuple_aligned(CurrentMemoryContext, cur, curlen, &mem) :
			 cur);
		s->onpage = onpage;
		s->xid = h.xid;
		s->frozen = h.urec < discard;
		s->st = s->frozen ? IHX_COMMITTED :
			iheap_xid_status(h.xid, InvalidBuffer, NULL);
		s->removable = s->frozen ||
			(s->st == IHX_COMMITTED &&
			 GlobalVisTestIsRemovableXid(vistest, h.xid, true));
		if (s->removable)
			break;				/* every snapshot sees this one or a newer one */

		/* step back to the previous version */
		rec = iheap_undo_fetch(rel, h.urec, true);
		if (rec == NULL)
		{
			/*
			 * Discarded under us: its writer is visible to all, so the
			 * version is, as if frozen.  (Discard applies aborted records
			 * before passing them, so an aborted version cannot lose its
			 * record while on the page; be safe if it did.)
			 */
			if (s->st == IHX_ABORTED)
				return -2;
			s->frozen = true;
			s->removable = true;
			s->st = IHX_COMMITTED;
			break;
		}
		if (!iheap_rebuild_prior(rel, &cur, &curlen, &h, rec, &mem))
		{
			/* the insert: no version before it */
			pfree(rec);
			*ended = true;
			break;
		}
		pfree(rec);
		onpage = false;
	}
	*stepsp = steps;
	return n;
}

/*
 * The fate of the marked entry dm of the row at off of the locked page buf,
 * as described for iheap_dm_check_entries(): the walk goes over the
 * versions newest first, over the ones some snapshot may still see; it
 * stops below a version whose creator every snapshot sees as committed.
 * Versions of aborted transactions are seen by nobody.
 *
 * The versions are collected with the table page locked, and compared with
 * the entry with it unlocked (*buf released; the caller reads it again):
 * index expressions and predicates are evaluated, which run the user's
 * functions.  The leaf page stays locked, so the row gets no new entry or
 * mark meanwhile, and the decision stays right: a writer that changes the
 * row after the versions were collected changes the index after that, once
 * the caller lets go of the leaf page.
 *
 * A key value stored out of line is fetched only for the version on the
 * page (if not aborted), whose TOAST data is live; an older version's value
 * may have been replaced by a change that is about to become visible to
 * all, after which VACUUM of the TOAST table may remove it.  Such a version
 * makes the answer KEEP, until the walk no longer reaches it.
 *
 * For an index with INCLUDE columns, an entry loses its mark only when
 * every snapshot sees the newest version, which then has the entry's key:
 * an unmarked entry's INCLUDE values are taken by index-only scans for
 * every version with its key.  If the newest version's INCLUDE values are
 * other, the answer is REWRITE, with the entry to put in its place.
 *
 * A KEEP answer sets *wait to the writer of the newest version that is not
 * aborted: once every snapshot sees that version, the walk stops there and
 * decides (dmbtree skips cleanup passes on the page until then, §21 D36).
 */
static DmbtEntryStatus
dm_entry_status(Relation rel, Buffer *buf, OffsetNumber off,
				const IHeapDmEntry *dm, GlobalVisState *vistest,
				TransactionId *horizon, TransactionId *wait,
				IndexTuple *newtup, MemoryContext resultcxt)
{
	bool		fetch_ok = ActiveSnapshotSet();
	bool		include = IndexRelationGetNumberOfAttributes(dm->index) >
		IndexRelationGetNumberOfKeyAttributes(dm->index);
	bool		newest = true;	/* no committed or running version above */
	bool		ended;
	DmWalkStep *steps;
	int			n;

	*wait = InvalidTransactionId;
	*newtup = NULL;
	n = dm_walk(rel, *buf, off, vistest, &steps, &ended);
	UnlockReleaseBuffer(*buf);
	*buf = InvalidBuffer;
	if (n == -1)
		return DMBT_ENTRY_DEAD;

	for (int i = 0; i < Max(n, 0); i++)
	{
		DmWalkStep *s = &steps[i];
		int			flags = IHEAP_DM_EVAL |
			((s->onpage && fetch_ok) ? IHEAP_DM_FETCH : 0);
		int			match;

		if (s->st == IHX_ABORTED)
			continue;
		if (!TransactionIdIsValid(*wait))
			*wait = s->xid;

		if (s->tuple != NULL)
		{
			match = iheap_dm_matches(rel, dm, s->tuple, flags);
			if (match < 0)
				return DMBT_ENTRY_KEEP;
			if (match > 0)
			{
				if (!newest || s->st != IHX_COMMITTED)
					return DMBT_ENTRY_KEEP;
				if (!include)
					return DMBT_ENTRY_CURRENT;
				if (!s->removable)
					return DMBT_ENTRY_KEEP;
				match = iheap_dm_matches(rel, dm, s->tuple,
										 flags | IHEAP_DM_INCLUDE);
				if (match < 0)
					return DMBT_ENTRY_KEEP;
				if (match > 0)
					return DMBT_ENTRY_CURRENT;
				if ((ih_hdr_infomask(s->tuple) & IHT_HASEXTERNAL) && !(flags & IHEAP_DM_FETCH))
					return DMBT_ENTRY_KEEP;
				else
				{
					Datum		ivalues[INDEX_MAX_KEYS];
					bool		iisnull[INDEX_MAX_KEYS];
					MemoryContext oldcxt;

					if (!iheap_dm_entry_values(rel, dm->index, s->tuple,
											   ivalues, iisnull))
						return DMBT_ENTRY_KEEP;
					oldcxt = MemoryContextSwitchTo(resultcxt);
					*newtup = index_form_tuple(RelationGetDescr(dm->index),
											   ivalues, iisnull);
					MemoryContextSwitchTo(oldcxt);
					return DMBT_ENTRY_REWRITE;
				}
			}
		}

		/* everybody sees this version or a newer one */
		if (s->removable)
		{
			if (!s->frozen &&
				(!TransactionIdIsValid(*horizon) ||
				 TransactionIdFollows(s->xid, *horizon)))
				*horizon = s->xid;
			return DMBT_ENTRY_DEAD;
		}
		newest = false;
	}
	/* the insert: no version before it ever had the key */
	return (n >= 0 && ended) ? DMBT_ENTRY_DEAD : DMBT_ENTRY_KEEP;
}

TransactionId
iheap_dm_horizon(Relation heaprel)
{
	/* also brings the GlobalVis state the walks test against up to date */
	return GetOldestNonRemovableTransactionId(heaprel);
}

/*
 * dmbtree's cleanup (DESIGN.md §11.5.6): for each delete-marked entry,
 *
 *	DEAD	 no snapshot can see a version of its row with its key: the row
 *			 is gone, or every version some snapshot may see has another key;
 *	CURRENT  the row's newest committed version has its key (and nobody is
 *			 changing the row): the mark can go;
 *	REWRITE  the same, but its INCLUDE values are other, and every snapshot
 *			 sees it: newtup takes the entry's place;
 *	KEEP	 otherwise.
 *
 * The caller holds the index leaf page locked, so the row cannot get a new
 * entry or mark meanwhile: a writer changes the table first (seen here as
 * a running version) and the index after (it waits for the leaf page).
 * entries[] is sorted by TID here.  Returns the conflict horizon for
 * standbys: the newest xid whose version hid a dead entry's key.
 */
TransactionId
iheap_dm_check_entries(Relation rel, Relation index, DmbtEntryCheck *entries,
					   int nentries)
{
	GlobalVisState *vistest = GlobalVisTestFor(rel);
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	TransactionId horizon = InvalidTransactionId;
	MemoryContext resultcxt = CurrentMemoryContext;
	MemoryContext cxt;
	MemoryContext oldcxt;

	qsort(entries, nentries, sizeof(DmbtEntryCheck), dm_check_cmp);

	cxt = AllocSetContextCreate(CurrentMemoryContext, "iheap dmbtree check",
								ALLOCSET_DEFAULT_SIZES);
	for (int i = 0; i < nentries; i++)
	{
		DmbtEntryCheck *e = &entries[i];
		BlockNumber blk = ItemPointerGetBlockNumber(&e->tid);
		IHeapDmEntry dm = {index, e->itup, true, true};
		Buffer		buf;

		e->wait = InvalidTransactionId;
		e->newtup = NULL;
		if (blk >= nblocks)
		{
			e->status = DMBT_ENTRY_DEAD;
			continue;
		}
		buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
		LockBuffer(buf, BUFFER_LOCK_SHARE);

		oldcxt = MemoryContextSwitchTo(cxt);
		e->status = dm_entry_status(rel, &buf, ItemPointerGetOffsetNumber(&e->tid),
									&dm, vistest, &horizon, &e->wait,
									&e->newtup, resultcxt);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(cxt);
		Assert(!BufferIsValid(buf));
	}
	MemoryContextDelete(cxt);

	return horizon;
}

/* ------------------------------------------------------------------------
 * Checking entries against their rows (dmbtree_check)
 * ------------------------------------------------------------------------
 */

static int
dm_verify_cmp(const void *a, const void *b)
{
	const DmbtEntryVerify *ea = (const DmbtEntryVerify *) a;
	const DmbtEntryVerify *eb = (const DmbtEntryVerify *) b;

	return ItemPointerCompare(&ea->tid, &eb->tid);
}

/*
 * Does the newest committed version of the row at "off" of the locked page
 * buf have the key (and INCLUDE values) of the unmarked entry dm?  Returns
 * 1 or 0; -1 when there is nothing to compare: no committed version (an
 * insert that is still running or was rolled back), or a deletion or move
 * (the row's entries wait for VACUUM), or a walk that cannot tell.  Returns
 * -2 when the comparison has to wait for the page to be unlocked (a value
 * stored out of line, an index expression or predicate): *copy (and
 * *copymem) are then set to a copy of the version.
 *
 * Versions of running transactions, ours included, and of aborted ones are
 * passed over, as in dm_entry_status(), which this follows.
 */
static int
dm_verify_unmarked(Relation rel, Buffer buf, OffsetNumber off,
				   const IHeapDmEntry *dm, char **copy, char **copymem)
{
	Page		page = BufferGetPage(buf);
	UndoRecPtr	discard = iheap_discard_ptr(rel);
	ItemId		lp = PageGetItemId(page, off);
	char	   *cur = (char *) page + ItemIdGetOffset(lp);
	uint32		curlen = ItemIdGetLength(lp);
	char	   *mem = NULL;
	bool		onpage = true;
	int			steps = 0;
	int			match;
	IHeapHdr	h;

	ih_hdr_read(cur, &h);
	for (;;)
	{
		IHeapUndoRec *rec;
		IHeapXidStatus st;
		char	   *newmem;

		if (++steps > 100000)
			return -1;
		if (h.urec < discard)
			break;				/* frozen: committed */

		/* a LOCK record in front of the version is transparent */
		if (h.infomask & IHT_LOCKED)
		{
			rec = iheap_undo_fetch(rel, h.urec, true);
			if (rec == NULL)
				break;			/* discarded under us: committed */
			if (rec->hdr.ur_type == IHU_LOCK)
			{
				IHeapHdr	ph;

				ih_hdr_read(rec->hdr.ur_prev_hdr, &ph);
				h.infomask = ph.infomask | (h.infomask & (IHT_XID_HINTS | IHT_STUB));
				h.urec = ph.urec;
				h.xid = ph.xid;
				pfree(rec);
				continue;
			}
			pfree(rec);
		}

		st = iheap_xid_status(h.xid, InvalidBuffer, NULL);
		if (st == IHX_COMMITTED)
			break;

		/* running, ours or aborted: the version before it */
		rec = iheap_undo_fetch(rel, h.urec, true);
		if (rec == NULL)
		{
			/* discarded under us: committed, unless it is aborted */
			if (st == IHX_ABORTED)
				return -1;
			break;
		}
		if (!iheap_rebuild_prior(rel, &cur, &curlen, &h, rec, &newmem))
		{
			pfree(rec);
			return -1;			/* the insert: nothing committed before it */
		}
		pfree(rec);
		if (mem)
			pfree(mem);
		mem = newmem;
		onpage = false;
	}

	if (h.infomask & (IHT_DELETED | IHT_MOVED))
		return -1;

	match = iheap_dm_matches(rel, dm, cur, IHEAP_DM_INCLUDE);
	if (match >= 0)
		return match;

	if (onpage)
		*copy = ih_copy_tuple_aligned(CurrentMemoryContext, cur, curlen, copymem);
	else
	{
		*copy = cur;
		*copymem = mem;
	}
	return -2;
}

/*
 * dmbtree_check(checkmarks): check entries of a leaf page against their
 * rows.  Every entry must point to a used line pointer of a data page:
 * line pointers become unused only after index vacuum removed the entries
 * pointing at them (§10.5), and index vacuum needs the leaf page, which the
 * caller holds share-locked.  An unmarked entry must match the newest
 * committed version of its row (§11.5.4), INCLUDE values too.  With the
 * leaf page locked, no entry of the rows can be added or marked meanwhile:
 * a writer changes a row first, a version not committed yet that this check
 * passes over, and the index after.
 *
 * A key stored out of line, or computed by an index expression, is compared
 * after the table page is unlocked (TOAST access takes locks and may read;
 * expressions run the user's functions), from a copy of the version: the
 * newest committed version's TOAST data stays while the leaf page is
 * locked.
 */
void
iheap_dm_verify_entries(Relation rel, Relation index, DmbtEntryVerify *entries,
						int nentries)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	Buffer		buf = InvalidBuffer;
	MemoryContext cxt;
	MemoryContext oldcxt;
	DmbtEntryVerify **later;
	char	  **latercopy;
	int			nlater = 0;

	qsort(entries, nentries, sizeof(DmbtEntryVerify), dm_verify_cmp);

	cxt = AllocSetContextCreate(CurrentMemoryContext, "iheap dmbtree verify",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);
	later = palloc(Max(nentries, 1) * sizeof(DmbtEntryVerify *));
	latercopy = palloc(Max(nentries, 1) * sizeof(char *));

	for (int i = 0; i < nentries; i++)
	{
		DmbtEntryVerify *e = &entries[i];
		BlockNumber blk = ItemPointerGetBlockNumber(&e->tid);
		OffsetNumber off = ItemPointerGetOffsetNumber(&e->tid);
		IHeapDmEntry dm = {index, e->itup, false, true};
		Page		page;
		ItemId		lp;
		char	   *copy;
		char	   *copymem = NULL;
		int			match;

		e->result = DMBT_VERIFY_OK;
		if (blk >= nblocks)
		{
			e->result = DMBT_VERIFY_NO_ROWS;
			continue;
		}
		if (!BufferIsValid(buf) || BufferGetBlockNumber(buf) != blk)
		{
			if (BufferIsValid(buf))
				UnlockReleaseBuffer(buf);
			buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
		}
		page = BufferGetPage(buf);
		if (!IHeapPageIsData(page))
		{
			e->result = DMBT_VERIFY_NO_ROWS;
			continue;
		}
		if (off < FirstOffsetNumber || off > PageGetMaxOffsetNumber(page) ||
			!ItemIdIsUsed(lp = PageGetItemId(page, off)))
		{
			e->result = DMBT_VERIFY_UNUSED;
			continue;
		}
		if (e->marked || !ItemIdIsNormal(lp))
			continue;			/* LP_DEAD: waiting for index vacuum */

		match = dm_verify_unmarked(rel, buf, off, &dm, &copy, &copymem);
		if (match == 0)
			e->result = DMBT_VERIFY_KEY;
		else if (match == -2)
		{
			later[nlater] = e;
			latercopy[nlater++] = copy;
		}
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);

	for (int i = 0; i < nlater; i++)
	{
		IHeapDmEntry dm = {index, later[i]->itup, false, true};

		if (iheap_dm_matches(rel, &dm, latercopy[i],
							 IHEAP_DM_UNLOCKED | IHEAP_DM_INCLUDE) == 0)
			later[i]->result = DMBT_VERIFY_KEY;
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}
