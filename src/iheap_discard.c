/*-------------------------------------------------------------------------
 *
 * iheap_discard.c
 *	  Discarding undo, with verification of aborted transactions.
 *
 * Discard walks the relation's undo stream from the discard pointer, one
 * logical page at a time, while every record on the page was written by a
 * transaction older than GetOldestNonRemovableTransactionId() (DESIGN.md
 * §21 D8, D9).  Before a page is passed, each record of an aborted (or
 * crashed) transaction is applied if its target item still shows that
 * transaction's version.  Hence nothing below the discard pointer can
 * describe an unapplied abort, and "undo pointer below the discard pointer"
 * means "frozen": committed and visible to everybody.
 *
 * One discarder at a time per relation (a heavyweight lock on the metapage
 * block); others skip.  Discard runs in backends that just allocated a new
 * undo page, in VACUUM, and via iheap_discard(regclass).
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "iheap.h"
#include "iheap_xlog.h"

#define DISCARD_MAX_PAGES_PER_CALL	1024

/*
 * An unforced discard stops at a page among the newest
 * IHEAP_DISCARD_OPEN_WINDOW logical pages that still has room: it is most
 * likely some backend's append page, and passing it would make that backend
 * allocate a new page for its next record (one page per transaction for
 * short transactions).  The window bounds the undo kept back this way.
 */
#define IHEAP_DISCARD_OPEN_WINDOW	16
#define IHEAP_UNDO_OPEN_MIN_FREE	256

typedef struct VerifyItem
{
	BlockNumber blkno;
	OffsetNumber offnum;
} VerifyItem;

/*
 * Discard what can be discarded.  Returns the number of logical pages
 * passed.  With force (VACUUM, iheap_discard()), waits for a concurrent
 * discarder instead of skipping, and passes open append pages too.
 */
int64
iheap_discard(Relation rel, bool force)
{
	Buffer		metabuf;
	Page		metapage;
	IHeapMetaPageData *meta;
	uint64		start_lpage;
	uint64		next_lpage;
	uint64		lp;
	TransactionId horizon;
	TransactionId latest_removed = InvalidTransactionId;
	IHeapExtent *ext;
	int			next;
	BlockNumber nblocks;
	MemoryContext cxt;
	MemoryContext oldcxt;
	int64		npages = 0;
	int64		nbytes = 0;		/* of the records on the pages passed */

	if (RecoveryInProgress())
		return 0;

	if (force)
		LockPage(rel, IHEAP_METAPAGE_BLKNO, ExclusiveLock);
	else if (!ConditionalLockPage(rel, IHEAP_METAPAGE_BLKNO, ExclusiveLock))
		return 0;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "iheap discard",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	horizon = GetOldestNonRemovableTransactionId(rel);

	/* snapshot of the stream bounds and extents */
	metabuf = ReadBufferExtended(rel, MAIN_FORKNUM, IHEAP_METAPAGE_BLKNO,
								 RBM_NORMAL, NULL);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	metapage = BufferGetPage(metabuf);
	meta = IHeapPageGetMeta(metapage);
	start_lpage = UndoRecPtrGetLPage(meta->im_discard);
	next_lpage = meta->im_next_lpage;
	next = meta->im_nextents;
	ext = palloc(Max(next, 1) * sizeof(IHeapExtent));
	memcpy(ext, meta->im_extents, next * sizeof(IHeapExtent));
	UnlockReleaseBuffer(metabuf);

	nblocks = RelationGetNumberOfBlocks(rel);

	for (lp = start_lpage; lp < next_lpage && npages < DISCARD_MAX_PAGES_PER_CALL; lp++)
	{
		BlockNumber blk = InvalidBlockNumber;
		Buffer		buf;
		Page		page;
		IHeapUndoPageHeaderData *uph;
		VerifyItem *verify = NULL;
		int			nverify = 0;
		uint32		off;

		CHECK_FOR_INTERRUPTS();

		for (int i = 0; i < next; i++)
		{
			if (ext[i].ie_lstart != IHEAP_EXTENT_FREE &&
				lp >= ext[i].ie_lstart && lp < ext[i].ie_lstart + ext[i].ie_npages)
			{
				blk = ext[i].ie_pstart + (BlockNumber) (lp - ext[i].ie_lstart);
				break;
			}
		}
		if (blk == InvalidBlockNumber || blk >= nblocks)
		{
			/* allocated but never written (crash): nothing on it */
			npages++;
			continue;
		}

		buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);

		if (IHeapPageGetType(page) != IHEAP_PAGE_UNDO ||
			IHeapUndoPageGetHeader(page)->uph_lpage != lp)
		{
			UnlockReleaseBuffer(buf);
			npages++;
			continue;
		}

		uph = IHeapUndoPageGetHeader(page);
		if (uph->uph_nrecs > 0 && TransactionIdIsNormal(uph->uph_max_xid) &&
			!TransactionIdPrecedes(uph->uph_max_xid, horizon))
		{
			UnlockReleaseBuffer(buf);
			break;
		}
		if (!force && lp + IHEAP_DISCARD_OPEN_WINDOW >= next_lpage &&
			(IHeapPageGetOpaque(page)->ihp_flags & IHU_DISCARDED) == 0 &&
			((PageHeader) page)->pd_upper - ((PageHeader) page)->pd_lower >=
			IHEAP_UNDO_OPEN_MIN_FREE)
		{
			UnlockReleaseBuffer(buf);
			break;
		}

		nbytes += ((PageHeader) page)->pd_lower - IHEAP_UNDO_DATA_START;

		/* no more appends to this page */
		if (BufferBeginSetHintBits(buf))
		{
			IHeapPageGetOpaque(page)->ihp_flags |= IHU_DISCARDED;
			BufferFinishSetHintBits(buf, true, true);
		}

		if (TransactionIdIsNormal(uph->uph_max_xid) &&
			(!TransactionIdIsValid(latest_removed) ||
			 TransactionIdFollows(uph->uph_max_xid, latest_removed)))
			latest_removed = uph->uph_max_xid;

		/* collect the targets of records written by aborted transactions */
		verify = palloc(sizeof(VerifyItem) * Max(uph->uph_nrecs, 1) * 4);
		off = IHEAP_UNDO_DATA_START;
		while (off + IHEAP_UNDOREC_HDRSZ <= ((PageHeader) page)->pd_lower)
		{
			IHeapUndoRecHeader hdr;

			memcpy(&hdr, (char *) page + off, IHEAP_UNDOREC_HDRSZ);
			if (hdr.ur_len < IHEAP_UNDOREC_HDRSZ)
				break;
			if (iheap_xid_status(hdr.ur_xid, InvalidBuffer, NULL) == IHX_ABORTED)
			{
				if (hdr.ur_type == IHU_MULTI_INSERT)
				{
					uint16		n;
					int			cap = Max(uph->uph_nrecs, 1) * 4;

					memcpy(&n, (char *) page + off + IHEAP_UNDOREC_HDRSZ, sizeof(uint16));
					if (nverify + n > cap)
						verify = repalloc(verify, sizeof(VerifyItem) * (nverify + n + cap));
					for (int i = 0; i < n; i++)
					{
						verify[nverify].blkno = hdr.ur_blkno;
						memcpy(&verify[nverify].offnum,
							   (char *) page + off + IHEAP_UNDOREC_HDRSZ +
							   sizeof(uint16) + i * sizeof(OffsetNumber),
							   sizeof(OffsetNumber));
						nverify++;
					}
				}
				else
				{
					if (nverify >= Max(uph->uph_nrecs, 1) * 4)
						verify = repalloc(verify, sizeof(VerifyItem) * (nverify * 2 + 4));
					verify[nverify].blkno = hdr.ur_blkno;
					verify[nverify].offnum = hdr.ur_offnum;
					nverify++;
				}
			}
			off += hdr.ur_len;
		}
		UnlockReleaseBuffer(buf);

		/* apply them (lock order: data page, then undo pages) */
		for (int i = 0; i < nverify; i++)
		{
			Buffer		dbuf;

			if (verify[i].blkno >= nblocks)
				continue;
			dbuf = ReadBufferExtended(rel, MAIN_FORKNUM, verify[i].blkno,
									  RBM_NORMAL, NULL);
			LockBuffer(dbuf, BUFFER_LOCK_EXCLUSIVE);
			if (IHeapPageIsData(BufferGetPage(dbuf)))
				(void) iheap_rollback_tuple(rel, dbuf, verify[i].offnum);
			UnlockReleaseBuffer(dbuf);
		}
		pfree(verify);
		npages++;
	}

	if (lp > start_lpage)
	{
		IHeapExtent released[IHEAP_MAX_EXTENTS];
		int			nreleased;

		metabuf = ReadBufferExtended(rel, MAIN_FORKNUM, IHEAP_METAPAGE_BLKNO,
									 RBM_NORMAL, NULL);
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
		metapage = BufferGetPage(metabuf);
		meta = IHeapPageGetMeta(metapage);

		START_CRIT_SECTION();
		if (MakeUndoRecPtr(lp, 0) > meta->im_discard)
			meta->im_discard = MakeUndoRecPtr(lp, 0);
		nreleased = iheap_meta_free_extents(meta, released, IHEAP_MAX_EXTENTS,
											lp - start_lpage);
		iheap_meta_set_lower(metapage);
		MarkBufferDirty(metabuf);

		if (RelationNeedsWAL(rel))
		{
			xl_iheap_discard xlrec;
			XLogRecPtr	recptr;

			xlrec.snapshotConflictHorizon = latest_removed;
			xlrec.flags = 0;
			XLogBeginInsert();
			XLogRegisterData(&xlrec, SizeOfIHeapDiscard);
			XLogRegisterBuffer(IH_BLK_META, metabuf, REGBUF_STANDARD);
			XLogRegisterBufData(IH_BLK_META, meta, iheap_meta_image_size(metapage));
			recptr = XLogInsert(RM_IHEAP_ID, XLOG_IHEAP_DISCARD);
			PageSetLSN(metapage, recptr);
		}
		END_CRIT_SECTION();

		MemoryContextSwitchTo(oldcxt);
		iheap_relcache_from_meta(rel, metapage);
		MemoryContextSwitchTo(cxt);
		UnlockReleaseBuffer(metabuf);

		/* released extent pages become free data pages */
		for (int i = 0; i < nreleased; i++)
		{
			for (uint32 p = 0; p < released[i].ie_npages; p++)
				RecordPageWithFreeSpace(rel, released[i].ie_pstart + p,
										BLCKSZ - SizeOfPageHeaderData - IHEAP_SPECIAL_SIZE);
			FreeSpaceMapVacuumRange(rel, released[i].ie_pstart,
									released[i].ie_pstart + released[i].ie_npages);
		}
	}

	UnlockPage(rel, IHEAP_METAPAGE_BLKNO, ExclusiveLock);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	iheap_stat_rollback_report(RelationGetRelid(rel), IHEAP_ROLLBACK_DISCARD);
	iheap_stat_count_discard(rel, npages, nbytes);

	return npages;
}

/*
 * Oldest xid referenced by live undo (records and covered creators), or
 * "horizon" if there is none older.  Used for relfrozenxid: tuples whose
 * undo pointer is at or above the discard pointer have xids no older.
 */
TransactionId
iheap_oldest_live_xid(Relation rel, TransactionId horizon)
{
	Buffer		metabuf;
	IHeapMetaPageData *meta;
	uint64		lp;
	uint64		next_lpage;
	IHeapExtent *ext;
	int			next;
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	TransactionId oldest = horizon;

	metabuf = ReadBufferExtended(rel, MAIN_FORKNUM, IHEAP_METAPAGE_BLKNO,
								 RBM_NORMAL, NULL);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	meta = IHeapPageGetMeta(BufferGetPage(metabuf));
	lp = UndoRecPtrGetLPage(meta->im_discard);
	next_lpage = meta->im_next_lpage;
	next = meta->im_nextents;
	ext = palloc(Max(next, 1) * sizeof(IHeapExtent));
	memcpy(ext, meta->im_extents, next * sizeof(IHeapExtent));
	UnlockReleaseBuffer(metabuf);

	for (; lp < next_lpage; lp++)
	{
		BlockNumber blk = InvalidBlockNumber;
		Buffer		buf;
		Page		page;

		for (int i = 0; i < next; i++)
		{
			if (ext[i].ie_lstart != IHEAP_EXTENT_FREE &&
				lp >= ext[i].ie_lstart && lp < ext[i].ie_lstart + ext[i].ie_npages)
			{
				blk = ext[i].ie_pstart + (BlockNumber) (lp - ext[i].ie_lstart);
				break;
			}
		}
		if (blk == InvalidBlockNumber || blk >= nblocks)
			continue;

		buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (IHeapPageGetType(page) == IHEAP_PAGE_UNDO &&
			IHeapUndoPageGetHeader(page)->uph_lpage == lp)
		{
			TransactionId x = IHeapUndoPageGetHeader(page)->uph_min_xid;

			if (TransactionIdIsNormal(x) && TransactionIdPrecedes(x, oldest))
				oldest = x;
		}
		UnlockReleaseBuffer(buf);
	}

	pfree(ext);
	return oldest;
}
