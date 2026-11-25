/*-------------------------------------------------------------------------
 *
 * iheap_vacuum.c
 *	  VACUUM for iheap.
 *
 * In-place updates create no dead tuples, so VACUUM has less to do than
 * for heap:
 *	1. discard undo (verifying aborted transactions);
 *	2. prune data pages: frozen deletions become LP_DEAD, committed ones
 *	   shrink to stubs, pages are compacted;
 *	3. remove index entries of LP_DEAD items, then make them LP_UNUSED;
 *	4. set visibility-map bits on pages whose items are all frozen;
 *	5. truncate empty trailing pages;
 *	6. advance relfrozenxid to the oldest xid referenced by live undo --
 *	   no freezing pass is ever needed.
 *
 * Steps 3 and 4 run the index AMs' bulk deletion and cleanup, in parallel
 * workers when VACUUM's PARALLEL option and the indexes allow it, through
 * core's parallel vacuum as for heap.
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/multixact.h"
#include "access/tidstore.h"
#include "access/transam.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "catalog/storage.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "iheap.h"
#include "iheap_xlog.h"
#include "dmbtree_api.h"

/*
 * The indexes VACUUM works on.  With a parallel vacuum (pvs) their bulk
 * deletion and cleanup are shared between parallel workers and this backend,
 * the dead items are in shared memory, and the indexes' statistics stay there
 * until parallel_vacuum_end() copies them to istats.
 */
typedef struct VacIndexes
{
	Relation   *indrels;
	int			nindexes;
	IndexBulkDeleteResult **istats;
	ParallelVacuumState *pvs;	/* NULL: this backend does it all */
	int			nscans;			/* bulk deletion passes so far */
} VacIndexes;

/* mark LP_DEAD items unused (vacuum second pass, also used by redo) */
void
iheap_page_set_unused(Page page, OffsetNumber *offs, int n)
{
	OffsetNumber maxoff;

	for (int i = 0; i < n; i++)
	{
		ItemId		lp = PageGetItemId(page, offs[i]);

		if (ItemIdIsDead(lp))
			ItemIdSetUnused(lp);
	}

	/* drop trailing unused line pointers */
	maxoff = PageGetMaxOffsetNumber(page);
	while (maxoff >= FirstOffsetNumber && !ItemIdIsUsed(PageGetItemId(page, maxoff)))
	{
		((PageHeader) page)->pd_lower -= sizeof(ItemIdData);
		maxoff--;
	}

	PageClearHasFreeLinePointers(page);
	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
	{
		if (!ItemIdIsUsed(PageGetItemId(page, off)))
		{
			PageSetHasFreeLinePointers(page);
			break;
		}
	}
}

/* can the page be marked all-visible (and all-frozen)? */
static bool
page_all_frozen(Relation rel, Page page, UndoRecPtr discard)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		lp = PageGetItemId(page, off);
		IHeapHdr	h;

		if (!ItemIdIsUsed(lp))
			continue;
		if (!ItemIdIsNormal(lp))
			return false;
		ih_hdr_read((char *) page + ItemIdGetOffset(lp), &h);
		if (h.infomask & (IHT_DELETED | IHT_MOVED | IHT_STUB | IHT_SPECULATIVE))
			return false;
		if (h.urec >= discard)
			return false;
	}
	return true;
}

/* set the VM bits of a page we hold exclusively locked */
static void
set_all_visible(Relation rel, Buffer buf, Buffer *vmbuf)
{
	Page		page = BufferGetPage(buf);
	BlockNumber blk = BufferGetBlockNumber(buf);
	uint8		flags = VISIBILITYMAP_ALL_VISIBLE | VISIBILITYMAP_ALL_FROZEN;

	if (PageIsAllVisible(page) &&
		(visibilitymap_get_status(rel, blk, vmbuf) & flags) == flags)
		return;

	visibilitymap_pin(rel, blk, vmbuf);
	LockBuffer(*vmbuf, BUFFER_LOCK_EXCLUSIVE);

	START_CRIT_SECTION();
	PageSetAllVisible(page);
	MarkBufferDirty(buf);
	(void) visibilitymap_set(blk, *vmbuf, flags, rel->rd_locator);

	if (RelationNeedsWAL(rel))
	{
		xl_iheap_visible xlrec;
		XLogRecPtr	recptr;

		xlrec.vmflags = flags;
		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfIHeapVisible);
		XLogRegisterBuffer(IH_BLK_DATA, buf, REGBUF_STANDARD);
		XLogRegisterBuffer(IH_BLK_VM, *vmbuf, 0);
		recptr = XLogInsert(RM_IHEAP_ID, XLOG_IHEAP_VISIBLE);
		PageSetLSN(page, recptr);
		PageSetLSN(BufferGetPage(*vmbuf), recptr);
	}
	END_CRIT_SECTION();

	LockBuffer(*vmbuf, BUFFER_LOCK_UNLOCK);
}

static void
log_unused(Relation rel, Buffer buf, OffsetNumber *offs, int n)
{
	xl_iheap_unused xlrec;
	XLogRecPtr	recptr;

	if (!RelationNeedsWAL(rel))
		return;
	xlrec.noffsets = n;
	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfIHeapUnused);
	XLogRegisterBuffer(IH_BLK_DATA, buf, REGBUF_STANDARD);
	XLogRegisterBufData(IH_BLK_DATA, offs, n * sizeof(OffsetNumber));
	recptr = XLogInsert(RM_IHEAP_ID, XLOG_IHEAP_UNUSED);
	PageSetLSN(BufferGetPage(buf), recptr);
}

static bool
page_is_empty(Page page)
{
	OffsetNumber maxoff;

	if (PageIsNew(page))
		return true;
	if (!IHeapPageIsData(page))
		return false;
	maxoff = PageGetMaxOffsetNumber(page);
	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
		if (ItemIdIsUsed(PageGetItemId(page, off)))
			return false;
	return true;
}

/* truncate trailing empty data pages that are not part of any undo extent */
static void
truncate_tail(Relation rel, BlockNumber nblocks)
{
	Buffer		metabuf;
	IHeapMetaPageData *meta;
	BlockNumber newnblocks = nblocks;
	BlockNumber limit = 1;

	if (nblocks <= 1)
		return;
	if (!ConditionalLockRelation(rel, AccessExclusiveLock))
		return;

	nblocks = RelationGetNumberOfBlocks(rel);
	newnblocks = nblocks;

	metabuf = ReadBufferExtended(rel, MAIN_FORKNUM, IHEAP_METAPAGE_BLKNO,
								 RBM_NORMAL, NULL);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	meta = IHeapPageGetMeta(BufferGetPage(metabuf));
	for (uint32 i = 0; i < meta->im_nextents; i++)
	{
		BlockNumber end = meta->im_extents[i].ie_pstart + meta->im_extents[i].ie_npages;

		if (end > limit)
			limit = end;
	}
	UnlockReleaseBuffer(metabuf);

	while (newnblocks > limit)
	{
		Buffer		buf;
		bool		empty;

		buf = ReadBufferExtended(rel, MAIN_FORKNUM, newnblocks - 1, RBM_NORMAL, NULL);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		empty = page_is_empty(BufferGetPage(buf));
		UnlockReleaseBuffer(buf);
		if (!empty)
			break;
		newnblocks--;
	}

	if (newnblocks < nblocks)
		RelationTruncate(rel, newnblocks);

	UnlockRelation(rel, AccessExclusiveLock);
}

/*
 * Start a parallel vacuum of the indexes if one can be had, else return NULL.
 * The conditions are heap's (dead_items_alloc() in vacuumlazy.c), and one of
 * iheap's own: dmbtree's cleanup of marked entries evaluates an index's
 * expressions and predicate on table rows, in parallel workers and in this
 * backend, which is in parallel mode until parallel_vacuum_end().  They must
 * be parallel safe then, as for a parallel build of the index.
 */
static ParallelVacuumState *
begin_parallel_vacuum(Relation rel, VacIndexes *vi, int nworkers,
					  int vac_work_mem, int elevel,
					  BufferAccessStrategy bstrategy)
{
	PlannerInfo *root;

	/* parallel workers cannot read temporary tables */
	if (RelationUsesLocalBuffers(rel))
	{
		/* warn only if workers were asked for, as heap does */
		if (nworkers > 0)
			ereport(WARNING,
					(errmsg("disabling parallel option of vacuum on \"%s\" --- cannot vacuum temporary tables in parallel",
							RelationGetRelationName(rel))));
		return NULL;
	}

	/* is_parallel_safe() looks at planner state: a dummy one, as for builds */
	root = makeNode(PlannerInfo);
	root->glob = makeNode(PlannerGlobal);
	for (int i = 0; i < vi->nindexes; i++)
	{
		Relation	index = vi->indrels[i];

		if (!dmbtree_is_dmbtree(index))
			continue;
		if (!is_parallel_safe(root, (Node *) RelationGetIndexExpressions(index)) ||
			!is_parallel_safe(root, (Node *) RelationGetIndexPredicate(index)))
		{
			/* as for temporary tables, but autovacuum would warn every run */
			if (nworkers > 0 && !AmAutoVacuumWorkerProcess())
				ereport(WARNING,
						(errmsg("disabling parallel option of vacuum on \"%s\" --- index \"%s\" has expressions or a predicate that are not parallel safe",
								RelationGetRelationName(rel),
								RelationGetRelationName(index))));
			return NULL;
		}
	}

	return parallel_vacuum_init(rel, vi->indrels, vi->nindexes, nworkers,
								vac_work_mem, elevel, bstrategy);
}

/* for an index's bulk deletion or cleanup by this backend alone */
static void
init_ivinfo(IndexVacuumInfo *ivinfo, Relation rel, Relation index,
			double live_tuples, BufferAccessStrategy bstrategy)
{
	ivinfo->index = index;
	ivinfo->heaprel = rel;
	ivinfo->analyze_only = false;
	ivinfo->is_autovacuum = AmAutoVacuumWorkerProcess();
	ivinfo->report_progress = false;
	ivinfo->estimated_count = true;
	ivinfo->message_level = DEBUG2;
	ivinfo->num_heap_tuples = live_tuples;
	ivinfo->strategy = bstrategy;
}

/*
 * Remove the index entries of the collected LP_DEAD items, then mark the
 * items unused (and the pages all-visible where possible).
 */
static void
vacuum_dead_items(Relation rel, VacIndexes *vi, TidStore *dead_items,
				  VacDeadItemsInfo *dead_info, double live_tuples,
				  BufferAccessStrategy bstrategy, UndoRecPtr discard,
				  Buffer *vmbuf, BlockNumber *nallvisible)
{
	TidStoreIter *iter;
	TidStoreIterResult *res;

	if (vi->pvs != NULL)
		parallel_vacuum_bulkdel_all_indexes(vi->pvs, live_tuples, vi->nscans,
											NULL);
	else
	{
		for (int i = 0; i < vi->nindexes; i++)
		{
			IndexVacuumInfo ivinfo;

			init_ivinfo(&ivinfo, rel, vi->indrels[i], live_tuples, bstrategy);
			vi->istats[i] = vac_bulkdel_one_index(&ivinfo, vi->istats[i],
												  dead_items, dead_info);
		}
	}
	vi->nscans++;

	iter = TidStoreBeginIterate(dead_items);
	while ((res = TidStoreIterateNext(iter)) != NULL)
	{
		OffsetNumber offs[MaxOffsetNumber];
		int			n;
		Buffer		buf;
		Page		page;

		vacuum_delay_point(false);
		n = TidStoreGetBlockOffsets(res, offs, lengthof(offs));
		buf = ReadBufferExtended(rel, MAIN_FORKNUM, res->blkno, RBM_NORMAL,
								 bstrategy);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		if (IHeapPageIsData(page))
		{
			START_CRIT_SECTION();
			iheap_page_set_unused(page, offs, n);
			MarkBufferDirty(buf);
			log_unused(rel, buf, offs, n);
			END_CRIT_SECTION();

			if (page_all_frozen(rel, page, discard))
			{
				set_all_visible(rel, buf, vmbuf);
				(*nallvisible)++;
			}
			RecordPageWithFreeSpace(rel, res->blkno, iheap_page_free_space(page));
		}
		UnlockReleaseBuffer(buf);
	}
	TidStoreEndIterate(iter);
}

void
iheap_vacuum_rel(Relation rel, const VacuumParams *params,
				 BufferAccessStrategy bstrategy)
{
	struct VacuumCutoffs cutoffs;
	TimestampTz starttime = GetCurrentTimestamp();
	BlockNumber nblocks;
	BlockNumber blk;
	BlockNumber nallvisible = 0;
	IHeapExtent *skip;
	int			nskip;
	int			nextskip = 0;
	BlockNumber vm_skipped = 0;
	BlockNumber norows = 1;		/* the metapage, undo, other non-data pages */
	bool		skip_all_visible =
		(params->options & VACOPT_DISABLE_PAGE_SKIPPING) == 0;

	/*
	 * INDEX_CLEANUP off, as for heap: indexes are left alone, so LP_DEAD
	 * items keep their index entries and stay LP_DEAD (and dmbtree indexes
	 * keep their delete-marked entries).
	 */
	bool		index_cleanup = params->index_cleanup != VACOPTVALUE_DISABLED;
	int			elevel = (params->options & VACOPT_VERBOSE) ? INFO : DEBUG2;
	int			vac_work_mem = AmAutoVacuumWorkerProcess() &&
		autovacuum_work_mem != -1 ?
		autovacuum_work_mem : maintenance_work_mem;
	VacIndexes	vi;
	TidStore   *dead_items;
	VacDeadItemsInfo *dead_info;
	double		live_tuples = 0;
	int64		dead_found = 0;
	Buffer		vmbuf = InvalidBuffer;
	TransactionId frozenxid;
	bool		frozenxid_updated;
	bool		minmulti_updated;
	UndoRecPtr	discard;
	MemoryContext cxt;
	MemoryContext oldcxt;

	(void) vacuum_get_cutoffs(rel, params, &cutoffs);

	cxt = AllocSetContextCreate(CurrentMemoryContext, "iheap vacuum",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	/* 1. discard undo; applies leftovers of aborted transactions */
	(void) iheap_discard_all(rel, true);
	discard = iheap_relcache(rel, true)->rc_discard;

	vac_open_indexes(rel, RowExclusiveLock, &vi.nindexes, &vi.indrels);
	vi.istats = palloc0(sizeof(IndexBulkDeleteResult *) * Max(vi.nindexes, 1));
	vi.nscans = 0;

	/*
	 * A parallel vacuum when PARALLEL does not turn it off and two indexes at
	 * least can share the work (one process vacuums an index), as for heap.
	 * From here to parallel_vacuum_end() this backend is in parallel mode:
	 * the scan below writes pages and WAL only, which parallel mode allows;
	 * catalog updates and truncation come after.
	 */
	vi.pvs = NULL;
	if (params->nworkers >= 0 && vi.nindexes > 1 && index_cleanup)
		vi.pvs = begin_parallel_vacuum(rel, &vi, params->nworkers,
									   vac_work_mem, elevel, bstrategy);
	if (vi.pvs != NULL)
		dead_items = parallel_vacuum_get_dead_items(vi.pvs, &dead_info);
	else
	{
		dead_info = palloc_object(VacDeadItemsInfo);
		dead_info->max_bytes = vac_work_mem * (Size) 1024;
		dead_info->num_items = 0;
		dead_items = TidStoreCreateLocal(dead_info->max_bytes, true);
	}

	/*
	 * 2. prune the data pages, collecting LP_DEAD items; 3. whenever the
	 * dead item store is full (and at the end), remove their index entries
	 * and free the line pointers.
	 */
	nblocks = RelationGetNumberOfBlocks(rel);
	nskip = iheap_nondata_ranges(rel, &skip);
	blk = 1;
	while (blk < nblocks)
	{
		for (; blk < nblocks; blk++)
		{
			Buffer		buf;
			Page		page;
			OffsetNumber dead[MaxOffsetNumber];
			int			ndead = 0;
			OffsetNumber maxoff;
			Size		freespace;

			/* undo extents (sorted by start) hold no data */
			while (nextskip < nskip &&
				   skip[nextskip].ie_pstart + skip[nextskip].ie_npages <= blk)
				nextskip++;
			if (nextskip < nskip && blk >= skip[nextskip].ie_pstart)
			{
				BlockNumber end = skip[nextskip].ie_pstart + skip[nextskip].ie_npages;

				norows += Min(end, nblocks) - blk;
				blk = end - 1;
				continue;
			}

			/*
			 * An all-visible page holds frozen live rows only (any change
			 * clears the bit): nothing to prune, collect or freeze.
			 */
			if (skip_all_visible && VM_ALL_VISIBLE(rel, blk, &vmbuf))
			{
				vm_skipped++;
				nallvisible++;
				continue;
			}

			vacuum_delay_point(false);

			buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_NORMAL, bstrategy);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			page = BufferGetPage(buf);
			if (!IHeapPageIsData(page))
			{
				norows++;
				UnlockReleaseBuffer(buf);
				continue;
			}

			(void) iheap_page_prune(rel, buf, false, NULL, NULL);

			maxoff = PageGetMaxOffsetNumber(page);
			for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
			{
				ItemId		lp = PageGetItemId(page, off);

				if (ItemIdIsDead(lp))
					dead[ndead++] = off;
				else if (ItemIdIsNormal(lp))
				{
					IHeapHdr	h;

					ih_hdr_read((char *) page + ItemIdGetOffset(lp), &h);
					if (!(h.infomask & (IHT_DELETED | IHT_MOVED)))
						live_tuples += 1;
				}
			}

			if (ndead > 0)
			{
				if (index_cleanup)
				{
					TidStoreSetBlockOffsets(dead_items, blk, dead, ndead);
					dead_info->num_items += ndead;
				}
				dead_found += ndead;
			}
			else if (page_all_frozen(rel, page, discard))
			{
				set_all_visible(rel, buf, &vmbuf);
				nallvisible++;
			}

			freespace = iheap_page_free_space(page);
			UnlockReleaseBuffer(buf);
			RecordPageWithFreeSpace(rel, blk, freespace);

			/* keep memory bounded: vacuum the indexes when the store is full */
			if (TidStoreMemoryUsage(dead_items) > dead_info->max_bytes)
			{
				blk++;
				break;
			}
		}

		if (dead_info->num_items > 0 && index_cleanup)
		{
			vacuum_dead_items(rel, &vi, dead_items, dead_info, live_tuples,
							  bstrategy, discard, &vmbuf, &nallvisible);
			if (vi.pvs != NULL)
			{
				parallel_vacuum_reset_dead_items(vi.pvs);
				dead_items = parallel_vacuum_get_dead_items(vi.pvs, &dead_info);
			}
			else
			{
				TidStoreDestroy(dead_items);
				dead_items = TidStoreCreateLocal(dead_info->max_bytes, true);
				dead_info->num_items = 0;
			}
		}
	}

	/* 4. index cleanup */
	if (vi.pvs != NULL)
		parallel_vacuum_cleanup_all_indexes(vi.pvs, live_tuples, vi.nscans,
											true, NULL);
	else
	{
		for (int i = 0; i < vi.nindexes && index_cleanup; i++)
		{
			IndexVacuumInfo ivinfo;

			init_ivinfo(&ivinfo, rel, vi.indrels[i], live_tuples, bstrategy);
			vi.istats[i] = vac_cleanup_one_index(&ivinfo, vi.istats[i]);
		}
	}

	/* out of parallel mode, where the catalogs can be updated */
	if (vi.pvs != NULL)
		parallel_vacuum_end(vi.pvs, vi.istats);	/* frees dead_items */
	else
		TidStoreDestroy(dead_items);
	for (int i = 0; i < vi.nindexes && index_cleanup; i++)
	{
		if (vi.istats[i] && !vi.istats[i]->estimated_count)
			vac_update_relstats(vi.indrels[i], vi.istats[i]->num_pages,
								vi.istats[i]->num_index_tuples, 0, 0, false,
								InvalidTransactionId, InvalidMultiXactId,
								NULL, NULL, false);
	}

	if (BufferIsValid(vmbuf))
		ReleaseBuffer(vmbuf);
	FreeSpaceMapVacuum(rel);

	/* 5. truncate empty tail */
	if (params->truncate != VACOPTVALUE_DISABLED)
		truncate_tail(rel, RelationGetNumberOfBlocks(rel));

	/*
	 * Rows on skipped pages: estimated from the old density, as heap does.
	 * Pages that cannot hold rows are left out of the sample, as heap leaves
	 * out its new pages.
	 */
	if (vm_skipped > 0)
		live_tuples = vac_estimate_reltuples(rel, nblocks, nblocks - vm_skipped,
											 norows, live_tuples);

	/* 6. relfrozenxid: oldest xid that live undo may make us look up */
	frozenxid = iheap_oldest_live_xid(rel, cutoffs.OldestXmin);
	vac_update_relstats(rel, RelationGetNumberOfBlocks(rel), live_tuples,
						nallvisible, nallvisible, vi.nindexes > 0,
						frozenxid, cutoffs.OldestMxact,
						&frozenxid_updated, &minmulti_updated, false);

	pgstat_report_vacuum(rel, (PgStat_Counter) live_tuples,
						 (PgStat_Counter) dead_found, starttime);

	vac_close_indexes(vi.nindexes, vi.indrels, NoLock);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}
