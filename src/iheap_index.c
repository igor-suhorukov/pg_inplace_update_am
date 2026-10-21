/*-------------------------------------------------------------------------
 *
 * iheap_index.c
 *	  Index support: build scans, CREATE INDEX CONCURRENTLY validation and
 *	  index tuple deletion checks.
 *
 * Under rule (b) every version of a row has the same values in the columns of
 * every existing index, so index entries never need per-version handling.  A
 * *new* index is different: versions still kept in undo for old snapshots may
 * differ in the new index's columns.  A build that sees in-place updates some
 * snapshot may not see yet therefore sets ii_BrokenHotChain, which makes core
 * set indcheckxmin so older snapshots don't use the new index.
 *
 * A build of a dmbtree index that takes in-place updates, new or rebuilt,
 * instead gives the index delete-marked entries for the keys of the older
 * versions some snapshot may still see: such an index serves every snapshot
 * at once.  Only when an older version holds a value stored out of line does
 * it fall back to indcheckxmin (dmbtbuild() sets it for a rebuild).
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "commands/progress.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"

#include "iheap.h"
#include "dmbtree_api.h"

typedef struct BuildEntry
{
	OffsetNumber off;
	char	   *tuple;
	uint32		len;
	bool		alive;
	TransactionId xmin;
	int			nolder;			/* older versions some snapshot may see */
	char	  **older;			/* newest first */
	uint32	   *olderlen;
} BuildEntry;

/*
 * Version of an item to index in a non-MVCC build, and whether it is alive
 * (for unique checks, recently dead rows are indexed but not alive).
 * Returns false if nothing should be indexed.
 */
static bool
build_version(Relation rel, Buffer buf, Page page, OffsetNumber off,
			  GlobalVisState *vistest, IHeapVersion *v, bool *alive,
			  bool *broken)
{
	ItemId		lp = PageGetItemId(page, off);
	char	   *tup = (char *) page + ItemIdGetOffset(lp);
	IHeapHdr	h;
	IHeapXidStatus st;
	SnapshotData any;

	ih_hdr_read(tup, &h);

	if (iheap_is_frozen(rel, &h))
	{
		if (h.infomask & (IHT_DELETED | IHT_MOVED))
			return false;
		v->tuple = tup;
		v->len = ItemIdGetLength(lp);
		v->mem = NULL;
		v->from_undo = false;
		v->xmin = h.xid;
		*alive = true;
		return true;
	}

	st = iheap_xid_status(h.xid, buf, tup);

	/*
	 * Some snapshot may still see an older version, which can differ in the
	 * index's columns if this one replaced it in place (or, for a deletion
	 * or move, whose header no longer tells, the version before it did).
	 */
	if ((h.infomask & (IHT_INPLACE | IHT_DELETED | IHT_MOVED)) &&
		!(st == IHX_COMMITTED &&
		  GlobalVisTestIsRemovableXid(vistest, h.xid, true)))
		*broken = true;

	any.snapshot_type = SNAPSHOT_ANY;
	if (!iheap_resolve_version(rel, buf, tup, ItemIdGetLength(lp), NULL, &any, v))
		return false;

	*alive = true;
	if (h.infomask & (IHT_DELETED | IHT_MOVED))
	{
		switch (st)
		{
			case IHX_COMMITTED:
				if (GlobalVisTestIsRemovableXid(vistest, h.xid, true))
				{
					if (v->mem)
						pfree(v->mem);
					return false;
				}
				*alive = false;
				break;
			case IHX_CURRENT:
				*alive = false;
				break;
			default:
				/* in progress (deletion may abort) or aborted (rolled back) */
				break;
		}
	}
	return true;
}

/*
 * For a build that indexes the keys of older versions: the versions of the
 * row at off before the one build_version() returns that some snapshot may
 * still see, newest first, copied into the current memory context.  The walk
 * stops at a version every snapshot sees (frozen, or committed and visible to
 * all): nobody sees the ones before it.  Versions of aborted transactions are
 * seen by nobody, deletions have no key.  Returns false when one of them
 * holds a value stored out of line: its key cannot be computed safely (the
 * TOAST data of a version on its way out may be removed meanwhile).
 */
static bool
older_versions(Relation rel, Page page, OffsetNumber off,
			   GlobalVisState *vistest, BuildEntry *e)
{
	ItemId		lp = PageGetItemId(page, off);
	char	   *cur = (char *) page + ItemIdGetOffset(lp);
	uint32		curlen = ItemIdGetLength(lp);
	UndoRecPtr	discard = iheap_discard_ptr(rel);
	bool		onpage = true;
	bool		indexed = false;	/* passed the version that is indexed */
	int			cap = 0;
	int			steps = 0;
	IHeapHdr	h;

	e->nolder = 0;
	ih_hdr_read(cur, &h);
	for (;;)
	{
		bool		deleted = (h.infomask & (IHT_DELETED | IHT_MOVED)) != 0;
		bool		removable;
		IHeapXidStatus st;
		IHeapUndoRec *rec;
		char	   *mem;

		if (++steps > 100000)
			return false;

		/* a LOCK record in front of the version is transparent */
		if (h.urec >= discard && (h.infomask & IHT_LOCKED))
		{
			rec = iheap_undo_fetch(rel, h.urec, true);
			if (rec != NULL && rec->hdr.ur_type == IHU_LOCK)
			{
				IHeapHdr	ph;

				ih_hdr_read(rec->hdr.ur_prev_hdr, &ph);
				h.infomask = ph.infomask | (h.infomask & (IHT_XID_HINTS | IHT_STUB));
				h.urec = ph.urec;
				h.xid = ph.xid;
				pfree(rec);
				continue;
			}
			if (rec != NULL)
				pfree(rec);
			else
				h.urec = InvalidUndoRecPtr; /* discarded under us */
		}

		if (h.urec < discard)
		{
			st = IHX_COMMITTED;
			removable = true;
		}
		else
		{
			st = iheap_xid_status(h.xid, InvalidBuffer, NULL);
			removable = (st == IHX_COMMITTED &&
						 GlobalVisTestIsRemovableXid(vistest, h.xid, true));
		}

		if (st != IHX_ABORTED && !deleted)
		{
			if (!indexed)
				indexed = true; /* the version build_version() returns */
			else
			{
				if (ih_hdr_infomask(cur) & IHT_HASEXTERNAL)
					return false;
				if (e->nolder == cap)
				{
					cap = Max(cap * 2, 4);
					if (e->nolder == 0)
					{
						e->older = palloc(cap * sizeof(char *));
						e->olderlen = palloc(cap * sizeof(uint32));
					}
					else
					{
						e->older = repalloc(e->older, cap * sizeof(char *));
						e->olderlen = repalloc(e->olderlen, cap * sizeof(uint32));
					}
				}
				e->older[e->nolder] = onpage ?
					ih_copy_tuple_aligned(CurrentMemoryContext, cur, curlen, &mem) :
					cur;
				e->olderlen[e->nolder++] = curlen;
			}
		}
		if (removable)
			break;				/* everybody sees this version or a newer one */

		rec = iheap_undo_fetch(rel, h.urec, true);
		if (rec == NULL)
			break;				/* discarded under us: visible to all */
		if (!iheap_rebuild_prior(rel, &cur, &curlen, &h, rec, &mem))
		{
			pfree(rec);
			break;				/* the insert */
		}
		pfree(rec);
		onpage = false;
	}
	return true;
}

/* are entry values a and b of index columns [from, to) the same, by image? */
static bool
build_values_equal(TupleDesc idesc, int from, int to, const Datum *a,
				   const bool *anull, const Datum *b, const bool *bnull)
{
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

/*
 * The entries of a row with older versions some snapshot may still see, in
 * a build that indexes their keys: the newest version's (in slot) as any
 * other, and a delete-marked one for each other key an older version has,
 * which cleanup removes once nobody sees that version.  An older version with
 * the newest one's key but other INCLUDE values makes the newest entry marked
 * too (a marked copy, which marks the entry after the load): an unmarked
 * entry's INCLUDE values are returned by index-only scans for every version
 * with its key.
 */
static void
build_older_keys(Relation indexRel, IndexInfo *indexInfo, EState *estate,
				 TupleTableSlot *slot, ExprState *predicate, BuildEntry *e,
				 ItemPointer tid, IndexBuildCallback callback,
				 void *callback_state)
{
	ExprContext *econtext = GetPerTupleExprContext(estate);
	TupleDesc	idesc = RelationGetDescr(indexRel);
	int			natts = IndexRelationGetNumberOfAttributes(indexRel);
	int			nkeyatts = IndexRelationGetNumberOfKeyAttributes(indexRel);
	Datum		newest[INDEX_MAX_KEYS];
	bool		newestnull[INDEX_MAX_KEYS];
	bool		newest_in;
	bool		mark_newest = false;
	Datum	  **keys = palloc(e->nolder * sizeof(Datum *));
	bool	  **keynulls = palloc(e->nolder * sizeof(bool *));
	int			nkeys = 0;
	ItemPointerData mtid = *tid;

	mtid.ip_posid |= DMBTREE_TID_MARK;

	/* the newest version's entry values, copied (each version resets) */
	newest_in = (predicate == NULL || ExecQual(predicate, econtext));
	if (newest_in)
	{
		FormIndexDatum(indexInfo, slot, estate, newest, newestnull);
		for (int a = 0; a < natts; a++)
		{
			CompactAttribute *att = TupleDescCompactAttr(idesc, a);

			if (!newestnull[a])
				newest[a] = datumCopy(newest[a], att->attbyval, att->attlen);
		}
	}

	for (int o = 0; o < e->nolder; o++)
	{
		Datum		values[INDEX_MAX_KEYS];
		bool		isnull[INDEX_MAX_KEYS];
		bool		seen = false;

		MemoryContextReset(econtext->ecxt_per_tuple_memory);
		iheap_store_tuple(slot, e->older[o], e->olderlen[o], false,
						  InvalidTransactionId, InvalidTransactionId,
						  InvalidCommandId);
		slot->tts_tid = *tid;
		if (predicate != NULL && !ExecQual(predicate, econtext))
			continue;
		FormIndexDatum(indexInfo, slot, estate, values, isnull);

		if (newest_in &&
			build_values_equal(idesc, 0, nkeyatts, values, isnull, newest,
							   newestnull))
		{
			if (!build_values_equal(idesc, nkeyatts, natts, values, isnull,
									newest, newestnull))
				mark_newest = true;
			continue;
		}
		for (int k = 0; k < nkeys && !seen; k++)
			seen = build_values_equal(idesc, 0, nkeyatts, values, isnull,
									  keys[k], keynulls[k]);
		if (seen)
			continue;

		callback(indexRel, &mtid, values, isnull, true, callback_state);
		keys[nkeys] = palloc(natts * sizeof(Datum));
		keynulls[nkeys] = palloc(natts * sizeof(bool));
		for (int a = 0; a < natts; a++)
		{
			CompactAttribute *att = TupleDescCompactAttr(idesc, a);

			keynulls[nkeys][a] = isnull[a];
			keys[nkeys][a] = isnull[a] ? (Datum) 0 :
				datumCopy(values[a], att->attbyval, att->attlen);
		}
		nkeys++;
	}

	if (newest_in)
	{
		callback(indexRel, tid, newest, newestnull, e->alive, callback_state);
		if (mark_newest)
			callback(indexRel, &mtid, newest, newestnull, true, callback_state);
	}
}

double
iheap_index_build_range_scan(Relation heapRel, Relation indexRel,
							 IndexInfo *indexInfo, bool allow_sync,
							 bool anyvisible, bool progress,
							 BlockNumber start_blockno, BlockNumber numblocks,
							 IndexBuildCallback callback, void *callback_state,
							 TableScanDesc scan)
{
	EState	   *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(heapRel),
													&TTSOpsIHeapTuple);
	ExprState  *predicate;
	Snapshot	snapshot;
	bool		unregister = false;
	bool		use_mvcc;
	ParallelBlockTableScanDesc pbscan = NULL;
	ParallelBlockTableScanWorkerData pbwork;
	BlockNumber nblocks = RelationGetNumberOfBlocks(heapRel);
	BlockNumber blk;
	BlockNumber endblk;
	BufferAccessStrategy strategy = GetAccessStrategy(BAS_BULKREAD);
	GlobalVisState *vistest = GlobalVisTestFor(heapRel);
	MemoryContext pagecxt;
	double		reltuples = 0;
	bool		broken = false;
	bool		olderkeys;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	econtext->ecxt_scantuple = slot;
	predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	if (scan != NULL)
	{
		snapshot = scan->rs_snapshot;
		pbscan = (ParallelBlockTableScanDesc) scan->rs_parallel;
	}
	else if (!IsBootstrapProcessingMode() && indexInfo->ii_Concurrent && !anyvisible)
	{
		snapshot = RegisterSnapshot(GetTransactionSnapshot());
		unregister = true;
	}
	else
		snapshot = SnapshotAny;
	use_mvcc = IsMVCCSnapshot(snapshot);

	/*
	 * A dmbtree index that takes in-place updates gets the keys of older
	 * versions too.  Not in a concurrent build, which waits for older
	 * snapshots before the index is used.
	 */
	olderkeys = !use_mvcc && !indexInfo->ii_Concurrent &&
		dmbtree_build_older_keys(indexRel, heapRel);

	pagecxt = AllocSetContextCreate(CurrentMemoryContext, "iheap index build page",
									ALLOCSET_DEFAULT_SIZES);

	if (pbscan != NULL)
	{
		table_block_parallelscan_startblock_init(heapRel, &pbwork, pbscan,
												 InvalidBlockNumber,
												 InvalidBlockNumber);
		blk = table_block_parallelscan_nextpage(heapRel, &pbwork, pbscan);
		endblk = InvalidBlockNumber;
	}
	else
	{
		blk = start_blockno;
		endblk = (numblocks == InvalidBlockNumber) ? nblocks :
			Min(nblocks, start_blockno + numblocks);
	}

	if (progress)
		pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_TOTAL,
									 pbscan ? pbscan->phs_nblocks : endblk);

	while (blk != InvalidBlockNumber && (pbscan != NULL || blk < endblk))
	{
		Buffer		buf;
		Page		page;
		BuildEntry	entries[IHEAP_MAX_ITEMS];
		int			nentries = 0;
		MemoryContext oldcxt;

		CHECK_FOR_INTERRUPTS();

		if (blk < nblocks)
		{
			buf = ReadBufferExtended(heapRel, MAIN_FORKNUM, blk, RBM_NORMAL, strategy);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			oldcxt = MemoryContextSwitchTo(pagecxt);
			if (IHeapPageIsData(page))
			{
				OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

				for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
				{
					ItemId		lp = PageGetItemId(page, off);
					IHeapVersion v;
					bool		alive = true;
					bool		rowbroken = false;
					bool		ok;

					if (!ItemIdIsNormal(lp))
						continue;
					if (use_mvcc)
						ok = iheap_resolve_version(heapRel, buf,
												   (char *) page + ItemIdGetOffset(lp),
												   ItemIdGetLength(lp), NULL,
												   snapshot, &v);
					else
						ok = build_version(heapRel, buf, page, off, vistest, &v,
										   &alive, &rowbroken);
					if (!ok)
						continue;

					/*
					 * Older versions some snapshot may see can have other
					 * keys: indexed too (marked), or indcheckxmin.
					 */
					entries[nentries].nolder = 0;
					if (rowbroken &&
						(!olderkeys ||
						 !older_versions(heapRel, page, off, vistest,
										 &entries[nentries])))
					{
						entries[nentries].nolder = 0;
						broken = true;
					}

					entries[nentries].off = off;
					if (v.from_undo)
						entries[nentries].tuple = v.tuple;
					else
					{
						char	   *mem;

						entries[nentries].tuple =
							ih_copy_tuple_aligned(pagecxt, v.tuple, v.len, &mem);
					}
					entries[nentries].len = v.len;
					entries[nentries].alive = alive;
					entries[nentries].xmin = v.xmin;
					nentries++;
				}
			}
			MemoryContextSwitchTo(oldcxt);
			UnlockReleaseBuffer(buf);

			for (int i = 0; i < nentries; i++)
			{
				ItemPointerData tid;

				MemoryContextReset(econtext->ecxt_per_tuple_memory);
				iheap_store_tuple(slot, entries[i].tuple, entries[i].len, false,
								  entries[i].xmin, InvalidTransactionId,
								  InvalidCommandId);
				ItemPointerSet(&tid, blk, entries[i].off);
				slot->tts_tid = tid;
				slot->tts_tableOid = RelationGetRelid(heapRel);

				if (entries[i].nolder > 0)
				{
					build_older_keys(indexRel, indexInfo, estate, slot,
									 predicate, &entries[i], &tid,
									 callback, callback_state);
					if (entries[i].alive)
						reltuples += 1;
					continue;
				}

				if (predicate != NULL && !ExecQual(predicate, econtext))
					continue;

				FormIndexDatum(indexInfo, slot, estate, values, isnull);
				callback(indexRel, &tid, values, isnull, entries[i].alive,
						 callback_state);
				if (entries[i].alive)
					reltuples += 1;
			}
			ExecClearTuple(slot);
			MemoryContextReset(pagecxt);
		}

		if (progress)
			pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE, blk + 1);

		if (pbscan != NULL)
			blk = table_block_parallelscan_nextpage(heapRel, &pbwork, pbscan);
		else
			blk++;
	}

	if (broken && !indexInfo->ii_Concurrent)
		indexInfo->ii_BrokenHotChain = true;

	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	MemoryContextDelete(pagecxt);
	FreeAccessStrategy(strategy);
	if (unregister)
		UnregisterSnapshot(snapshot);

	/* the caller's scan (parallel builds) is ended by the caller */
	if (scan != NULL)
		table_endscan(scan);

	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;

	return reltuples;
}

/*
 * CREATE INDEX CONCURRENTLY, last phase: insert the rows the index is
 * missing.  state->tuplesort holds the index's TIDs, sorted.
 */
void
iheap_index_validate_scan(Relation heapRel, Relation indexRel,
						  IndexInfo *indexInfo, Snapshot snapshot,
						  ValidateIndexState *state)
{
	TableScanDesc scan;
	TupleTableSlot *slot;
	EState	   *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	ExprState  *predicate;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	ItemPointerData decoded;
	ItemPointer indexcursor = NULL;
	bool		tuplesort_empty = false;

	slot = MakeSingleTupleTableSlot(RelationGetDescr(heapRel), &TTSOpsIHeapTuple);
	econtext->ecxt_scantuple = slot;
	predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	scan = table_beginscan_strat(heapRel, snapshot, 0, NULL, true, false);

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		ItemPointer heapcursor = &slot->tts_tid;

		CHECK_FOR_INTERRUPTS();
		state->htups += 1;

		while (!tuplesort_empty &&
			   (!indexcursor || ItemPointerCompare(indexcursor, heapcursor) < 0))
		{
			Datum		ts_val;
			bool		ts_isnull;

			tuplesort_empty = !tuplesort_getdatum(state->tuplesort, true, false,
												  &ts_val, &ts_isnull, NULL);
			if (!tuplesort_empty)
			{
				itemptr_decode(&decoded, DatumGetInt64(ts_val));
				indexcursor = &decoded;
			}
			else
				indexcursor = NULL;
		}

		if (tuplesort_empty || ItemPointerCompare(indexcursor, heapcursor) > 0)
		{
			ItemPointerData tid = *heapcursor;

			MemoryContextReset(econtext->ecxt_per_tuple_memory);
			if (predicate != NULL && !ExecQual(predicate, econtext))
				continue;
			FormIndexDatum(indexInfo, slot, estate, values, isnull);
			index_insert(indexRel, values, isnull, &tid, heapRel,
						 indexInfo->ii_Unique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
						 false, indexInfo);
			state->tups_inserted += 1;
		}
	}

	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);

	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;
}

/*
 * Which of the index entries' TIDs point to rows no snapshot can see?
 * Returns the newest xid whose effects are removed (for standby conflicts).
 */
TransactionId
iheap_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	TransactionId horizon = InvalidTransactionId;
	GlobalVisState *vistest = GlobalVisTestFor(rel);
	Buffer		buf = InvalidBuffer;
	BlockNumber curblk = InvalidBlockNumber;
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);

	for (int i = 0; i < delstate->ndeltids; i++)
	{
		TM_IndexDelete *d = &delstate->deltids[i];
		TM_IndexStatus *st = &delstate->status[d->id];
		BlockNumber blk = ItemPointerGetBlockNumber(&d->tid);
		OffsetNumber off = ItemPointerGetOffsetNumber(&d->tid);
		Page		page;
		bool		deletable = false;

		if (blk >= nblocks)
		{
			st->knowndeletable = true;
			continue;
		}
		if (blk != curblk)
		{
			if (BufferIsValid(buf))
				UnlockReleaseBuffer(buf);
			buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			curblk = blk;
		}
		page = BufferGetPage(buf);

		if (!IHeapPageIsData(page) || off > PageGetMaxOffsetNumber(page))
			deletable = true;
		else
		{
			ItemId		lp = PageGetItemId(page, off);

			if (!ItemIdIsUsed(lp) || ItemIdIsDead(lp))
				deletable = true;
			else if (ItemIdIsNormal(lp))
			{
				char	   *tup = (char *) page + ItemIdGetOffset(lp);
				IHeapHdr	h;

				ih_hdr_read(tup, &h);
				if (h.infomask & (IHT_DELETED | IHT_MOVED))
				{
					if (iheap_is_frozen(rel, &h))
						deletable = true;
					else if (iheap_xid_status(h.xid, buf, tup) == IHX_COMMITTED &&
							 GlobalVisTestIsRemovableXid(vistest, h.xid, true))
					{
						deletable = true;
						if (!TransactionIdIsValid(horizon) ||
							TransactionIdFollows(h.xid, horizon))
							horizon = h.xid;
					}
				}
			}
		}

		if (deletable)
			st->knowndeletable = true;
	}

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);

	/*
	 * Return only the deletable entries (in any order): index AMs rely on
	 * ndeltids being 0 when nothing can be deleted (bottom-up deletion), and
	 * entries known to be deletable on entry stay deletable.
	 */
	{
		int			ndeletable = 0;

		for (int i = 0; i < delstate->ndeltids; i++)
		{
			if (delstate->status[delstate->deltids[i].id].knowndeletable)
			{
				TM_IndexDelete tmp = delstate->deltids[i];

				delstate->deltids[i] = delstate->deltids[ndeletable];
				delstate->deltids[ndeletable] = tmp;
				ndeletable++;
			}
		}
		Assert(ndeletable > 0 || delstate->bottomup);
		delstate->ndeltids = ndeletable;
	}

	return horizon;
}
