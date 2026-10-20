/*-------------------------------------------------------------------------
 *
 * iheap_vacuum.c
 *	  VACUUM for iheap.
 *
 * In-place updates create no dead tuples, so VACUUM has less to do than
 * for heap (DESIGN.md §10.2):
 *	1. discard undo (verifying aborted transactions);
 *	2. prune data pages: frozen deletions become LP_DEAD, committed ones
 *	   shrink to stubs, pages are compacted;
 *	3. remove index entries of LP_DEAD items, then make them LP_UNUSED;
 *	4. set visibility-map bits on pages whose items are all frozen;
 *	5. truncate empty trailing pages;
 *	6. advance relfrozenxid to the oldest xid referenced by live undo --
 *	   no freezing pass is ever needed (§10.6).
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
 * Remove the index entries of the collected LP_DEAD items, then mark the
 * items unused (and the pages all-visible where possible).
 */
static void
vacuum_dead_items(Relation rel, Relation *indrels, int nindexes,
				  IndexBulkDeleteResult **istats, TidStore *dead_items,
				  VacDeadItemsInfo *dead_info, double live_tuples,
				  BufferAccessStrategy bstrategy, UndoRecPtr discard,
				  Buffer *vmbuf, BlockNumber *nallvisible)
{
	TidStoreIter *iter;
	TidStoreIterResult *res;

	for (int i = 0; i < nindexes; i++)
	{
		IndexVacuumInfo ivinfo;

		ivinfo.index = indrels[i];
		ivinfo.heaprel = rel;
		ivinfo.analyze_only = false;
		ivinfo.is_autovacuum = AmAutoVacuumWorkerProcess();
		ivinfo.report_progress = false;
		ivinfo.estimated_count = true;
		ivinfo.message_level = DEBUG2;
		ivinfo.num_heap_tuples = live_tuples;
		ivinfo.strategy = bstrategy;
		istats[i] = vac_bulkdel_one_index(&ivinfo, istats[i], dead_items,
										  dead_info);
	}

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
	bool		skip_all_visible =
		(params->options & VACOPT_DISABLE_PAGE_SKIPPING) == 0;
	int			nindexes;
	Relation   *indrels;
	TidStore   *dead_items;
	VacDeadItemsInfo dead_info;
	IndexBulkDeleteResult **istats;
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
	(void) iheap_discard(rel, true);
	discard = iheap_relcache(rel, true)->rc_discard;

	vac_open_indexes(rel, RowExclusiveLock, &nindexes, &indrels);
	istats = palloc0(sizeof(IndexBulkDeleteResult *) * Max(nindexes, 1));

	dead_info.max_bytes = (size_t) maintenance_work_mem * 1024;
	dead_info.num_items = 0;
	dead_items = TidStoreCreateLocal(dead_info.max_bytes, true);

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
				blk = skip[nextskip].ie_pstart + skip[nextskip].ie_npages - 1;
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
				TidStoreSetBlockOffsets(dead_items, blk, dead, ndead);
				dead_info.num_items += ndead;
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
			if (TidStoreMemoryUsage(dead_items) > dead_info.max_bytes)
			{
				blk++;
				break;
			}
		}

		if (dead_info.num_items > 0)
		{
			vacuum_dead_items(rel, indrels, nindexes, istats, dead_items,
							  &dead_info, live_tuples, bstrategy, discard,
							  &vmbuf, &nallvisible);
			TidStoreDestroy(dead_items);
			dead_items = TidStoreCreateLocal(dead_info.max_bytes, true);
			dead_info.num_items = 0;
		}
	}

	/* 4. index cleanup */
	for (int i = 0; i < nindexes; i++)
	{
		IndexVacuumInfo ivinfo;

		ivinfo.index = indrels[i];
		ivinfo.heaprel = rel;
		ivinfo.analyze_only = false;
		ivinfo.is_autovacuum = AmAutoVacuumWorkerProcess();
		ivinfo.report_progress = false;
		ivinfo.estimated_count = true;
		ivinfo.message_level = DEBUG2;
		ivinfo.num_heap_tuples = live_tuples;
		ivinfo.strategy = bstrategy;
		istats[i] = vac_cleanup_one_index(&ivinfo, istats[i]);
		if (istats[i] && !istats[i]->estimated_count)
			vac_update_relstats(indrels[i], istats[i]->num_pages,
								istats[i]->num_index_tuples, 0, 0, false,
								InvalidTransactionId, InvalidMultiXactId,
								NULL, NULL, false);
	}

	if (BufferIsValid(vmbuf))
		ReleaseBuffer(vmbuf);
	TidStoreDestroy(dead_items);
	FreeSpaceMapVacuum(rel);

	/* 5. truncate empty tail */
	if (params->truncate != VACOPTVALUE_DISABLED)
		truncate_tail(rel, RelationGetNumberOfBlocks(rel));

	/* rows on skipped pages: estimated from the old density, as heap does */
	if (vm_skipped > 0)
		live_tuples = vac_estimate_reltuples(rel, nblocks, nblocks - vm_skipped,
											 live_tuples);

	/* 6. relfrozenxid: oldest xid that live undo may make us look up */
	frozenxid = iheap_oldest_live_xid(rel, cutoffs.OldestXmin);
	vac_update_relstats(rel, RelationGetNumberOfBlocks(rel), live_tuples,
						nallvisible, nallvisible, nindexes > 0,
						frozenxid, cutoffs.OldestMxact,
						&frozenxid_updated, &minmulti_updated, false);

	pgstat_report_vacuum(rel, (PgStat_Counter) live_tuples,
						 (PgStat_Counter) dead_found, starttime);

	vac_close_indexes(nindexes, indrels, NoLock);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}
