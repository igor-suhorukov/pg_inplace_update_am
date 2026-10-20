/*-------------------------------------------------------------------------
 *
 * iheap_xlog.c
 *	  The iheap custom WAL resource manager: redo, desc, identify, mask
 *	  (decode is in iheap_logical.c).
 *
 * Redo is deterministic: it applies the same page routines the primary
 * used (item placement, compaction, undo record placement), so standbys
 * and recovered pages are byte-identical except for masked hint bits.
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/visibilitymap.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogutils.h"
#include "storage/bufmgr.h"
#include "storage/standby.h"
#include "utils/rel.h"

#include "iheap.h"
#include "iheap_xlog.h"

int			iheap_logical_decoding = IHEAP_LOGICAL_ON;

/* ------------------------------------------------------------------------
 * Redo helpers
 * ------------------------------------------------------------------------
 */
/* clear the VM bits of heapblk (and of heapblk2, if valid: same VM page) */
static void
redo_vm_clear2(XLogReaderState *record, uint8 block_id, BlockNumber heapblk,
			   BlockNumber heapblk2, RelFileLocator rlocator)
{
	Buffer		vmbuf = InvalidBuffer;
	Relation	reln;

	if (!XLogRecHasBlockRef(record, block_id))
		return;
	reln = CreateFakeRelcacheEntry(rlocator);
	if (XLogReadBufferForRedo(record, block_id, &vmbuf) == BLK_NEEDS_REDO)
	{
		bool		cleared;

		cleared = visibilitymap_clear(reln, heapblk, vmbuf,
									  VISIBILITYMAP_VALID_BITS);
		if (heapblk2 != InvalidBlockNumber &&
			visibilitymap_clear(reln, heapblk2, vmbuf, VISIBILITYMAP_VALID_BITS))
			cleared = true;
		if (cleared)
			PageSetLSN(BufferGetPage(vmbuf), record->EndRecPtr);
	}
	if (BufferIsValid(vmbuf))
		UnlockReleaseBuffer(vmbuf);
	FreeFakeRelcacheEntry(reln);
}

static void
redo_vm_clear(XLogReaderState *record, uint8 block_id, BlockNumber heapblk,
			  RelFileLocator rlocator)
{
	redo_vm_clear2(record, block_id, heapblk, InvalidBlockNumber, rlocator);
}

/* apply the undo records carried by block_id to the undo page */
static void
redo_undo_block(XLogReaderState *record, uint8 block_id)
{
	Buffer		buf;

	if (!XLogRecHasBlockRef(record, block_id))
		return;
	if (XLogReadBufferForRedo(record, block_id, &buf) == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buf);
		Size		len;
		char	   *data = XLogRecGetBlockData(record, block_id, &len);

		iheap_undo_redo_records(record, page, data, len);
		PageSetLSN(page, record->EndRecPtr);
		MarkBufferDirty(buf);
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

/* replace the metapage contents with the logged image */
static void
redo_meta_block(XLogReaderState *record, uint8 block_id)
{
	Buffer		buf;

	if (XLogReadBufferForRedo(record, block_id, &buf) == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buf);
		Size		len;
		char	   *data = XLogRecGetBlockData(record, block_id, &len);
		IHeapMetaPageData *meta = IHeapPageGetMeta(page);

		memcpy(meta, data, len);
		iheap_meta_set_lower(page);
		PageSetLSN(page, record->EndRecPtr);
		MarkBufferDirty(buf);
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

static Page
redo_data_block(XLogReaderState *record, uint8 block_id, bool init,
				Buffer *buf, XLogRedoAction *action)
{
	if (init)
	{
		*buf = XLogInitBufferForRedo(record, block_id);
		iheap_init_data_page(BufferGetPage(*buf));
		*action = BLK_NEEDS_REDO;
	}
	else
		*action = XLogReadBufferForRedo(record, block_id, buf);
	return BufferIsValid(*buf) ? BufferGetPage(*buf) : NULL;
}

static void
redo_finish(XLogReaderState *record, Buffer buf, XLogRedoAction action)
{
	if (action == BLK_NEEDS_REDO)
	{
		PageSetLSN(BufferGetPage(buf), record->EndRecPtr);
		MarkBufferDirty(buf);
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

/* ------------------------------------------------------------------------
 * Redo routines
 * ------------------------------------------------------------------------
 */
static void
redo_insert(XLogReaderState *record)
{
	xl_iheap_insert *xlrec = (xl_iheap_insert *) XLogRecGetData(record);
	RelFileLocator rlocator;
	BlockNumber blk;
	Buffer		buf;
	XLogRedoAction action;
	Page		page;

	XLogRecGetBlockTag(record, IH_BLK_DATA, &rlocator, NULL, &blk);
	if (xlrec->flags & XLIH_VM_CLEARED)
		redo_vm_clear(record, IH_BLK_VM, blk, rlocator);

	page = redo_data_block(record, IH_BLK_DATA, (xlrec->flags & XLIH_INIT_PAGE) != 0,
						   &buf, &action);
	if (action == BLK_NEEDS_REDO)
	{
		Size		len;
		char	   *tup = XLogRecGetBlockData(record, IH_BLK_DATA, &len);

		if (iheap_page_add_item(page, tup, len, xlrec->offnum) != xlrec->offnum)
			elog(PANIC, "iheap redo: failed to add tuple");
		if (xlrec->flags & XLIH_VM_CLEARED)
			PageClearAllVisible(page);
	}
	redo_finish(record, buf, action);
	redo_undo_block(record, IH_BLK_UNDO);
}

static void
redo_multi_insert(XLogReaderState *record)
{
	xl_iheap_multi_insert *xlrec = (xl_iheap_multi_insert *) XLogRecGetData(record);
	RelFileLocator rlocator;
	BlockNumber blk;
	Buffer		buf;
	XLogRedoAction action;
	Page		page;

	XLogRecGetBlockTag(record, IH_BLK_DATA, &rlocator, NULL, &blk);
	if (xlrec->flags & XLIH_VM_CLEARED)
		redo_vm_clear(record, IH_BLK_VM, blk, rlocator);

	page = redo_data_block(record, IH_BLK_DATA, (xlrec->flags & XLIH_INIT_PAGE) != 0,
						   &buf, &action);
	if (action == BLK_NEEDS_REDO)
	{
		Size		len;
		char	   *data = XLogRecGetBlockData(record, IH_BLK_DATA, &len);

		for (int i = 0; i < xlrec->ntuples; i++)
		{
			OffsetNumber off;
			uint16		tlen;

			memcpy(&off, data, sizeof(OffsetNumber));
			data += sizeof(OffsetNumber);
			memcpy(&tlen, data, sizeof(uint16));
			data += sizeof(uint16);
			if (iheap_page_add_item(page, data, tlen, off) != off)
				elog(PANIC, "iheap redo: failed to add tuple");
			data += tlen;
		}
		if (xlrec->flags & XLIH_VM_CLEARED)
			PageClearAllVisible(page);
	}
	redo_finish(record, buf, action);
	redo_undo_block(record, IH_BLK_UNDO);
}

/* DELETE and LOCK: replace the item's header */
static void
redo_hdrchange(XLogReaderState *record, bool prunable)
{
	xl_iheap_hdrchange *xlrec = (xl_iheap_hdrchange *) XLogRecGetData(record);
	RelFileLocator rlocator;
	BlockNumber blk;
	Buffer		buf;
	XLogRedoAction action;

	XLogRecGetBlockTag(record, IH_BLK_DATA, &rlocator, NULL, &blk);
	if (xlrec->flags & XLIH_VM_CLEARED)
		redo_vm_clear(record, IH_BLK_VM, blk, rlocator);

	action = XLogReadBufferForRedo(record, IH_BLK_DATA, &buf);
	if (action == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buf);
		ItemId		lp = PageGetItemId(page, xlrec->offnum);
		Size		len;
		char	   *hdr = XLogRecGetBlockData(record, IH_BLK_DATA, &len);

		memcpy((char *) page + ItemIdGetOffset(lp), hdr, IHEAP_TUPHDR_SIZE);
		if (prunable)
			IHeapPageGetOpaque(page)->ihp_flags |= IHP_PRUNABLE;
		if (xlrec->flags & XLIH_VM_CLEARED)
			PageClearAllVisible(page);
	}
	redo_finish(record, buf, action);
	redo_undo_block(record, IH_BLK_UNDO);
}

static void
redo_update(XLogReaderState *record)
{
	xl_iheap_update *xlrec = (xl_iheap_update *) XLogRecGetData(record);
	RelFileLocator rlocator;
	BlockNumber blk;
	Buffer		buf;
	XLogRedoAction action;

	XLogRecGetBlockTag(record, IH_BLK_DATA, &rlocator, NULL, &blk);
	if (xlrec->flags & XLIH_VM_CLEARED)
		redo_vm_clear(record, IH_BLK_VM, blk, rlocator);

	action = XLogReadBufferForRedo(record, IH_BLK_DATA, &buf);
	if (action == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buf);
		ItemId		lp = PageGetItemId(page, xlrec->offnum);
		char	   *old = (char *) page + ItemIdGetOffset(lp);
		uint32		oldlen = ItemIdGetLength(lp);
		Size		len;
		char	   *data = XLogRecGetBlockData(record, IH_BLK_DATA, &len);
		uint32		newmid = len - IHEAP_TUPHDR_SIZE;
		PGAlignedBlock tbuf;
		char	   *t = tbuf.data;

		if ((uint32) xlrec->prefix + xlrec->suffix + IHEAP_TUPHDR_SIZE > oldlen ||
			IHEAP_TUPHDR_SIZE + xlrec->prefix + newmid + xlrec->suffix != xlrec->new_len)
			elog(PANIC, "iheap redo: update does not match the page");

		memcpy(t, data, IHEAP_TUPHDR_SIZE);
		memcpy(t + IHEAP_TUPHDR_SIZE, old + IHEAP_TUPHDR_SIZE, xlrec->prefix);
		memcpy(t + IHEAP_TUPHDR_SIZE + xlrec->prefix, data + IHEAP_TUPHDR_SIZE, newmid);
		memcpy(t + IHEAP_TUPHDR_SIZE + xlrec->prefix + newmid,
			   old + oldlen - xlrec->suffix, xlrec->suffix);

		if (xlrec->flags & XLIH_RELOCATED)
		{
			if (!iheap_page_place_item_ext(page, xlrec->offnum, t, xlrec->new_len,
										   Max(xlrec->capacity, xlrec->new_len)))
				elog(PANIC, "iheap redo: relocation failed");
			if (ItemIdGetOffset(lp) != xlrec->lp_off)
				elog(PANIC, "iheap redo: relocation diverged (%u vs %u)",
					 ItemIdGetOffset(lp), xlrec->lp_off);
		}
		else
		{
			memcpy(old, t, xlrec->new_len);
			ItemIdSetNormal(lp, ItemIdGetOffset(lp), xlrec->new_len);
		}
		if ((xlrec->flags & XLIH_RELOCATED) || xlrec->new_len < oldlen)
			IHeapPageGetOpaque(page)->ihp_flags |= IHP_PRUNABLE;
		if (xlrec->flags & XLIH_VM_CLEARED)
			PageClearAllVisible(page);
	}
	redo_finish(record, buf, action);
	redo_undo_block(record, IH_BLK_UNDO);
}

static void
redo_move(XLogReaderState *record)
{
	xl_iheap_move *xlrec = (xl_iheap_move *) XLogRecGetData(record);
	RelFileLocator rlocator;
	BlockNumber oldblk;
	BlockNumber newblk = InvalidBlockNumber;
	bool		same = (xlrec->flags & XLIH_SAME_PAGE) != 0;
	Buffer		buf;
	XLogRedoAction action;

	XLogRecGetBlockTag(record, IH_BLK_DATA, &rlocator, NULL, &oldblk);
	if (!same)
		XLogRecGetBlockTag(record, IH_BLK_NEWDATA, NULL, NULL, &newblk);

	if ((xlrec->flags & XLIH_NEW_VM_CLEARED) &&
		!XLogRecHasBlockRef(record, IH_BLK_NEWVM))
	{
		/* both pages' bits are on one VM page, registered as VM */
		redo_vm_clear2(record, IH_BLK_VM,
					   (xlrec->flags & XLIH_VM_CLEARED) ? oldblk : newblk,
					   (xlrec->flags & XLIH_VM_CLEARED) ? newblk : InvalidBlockNumber,
					   rlocator);
	}
	else
	{
		if (xlrec->flags & XLIH_VM_CLEARED)
			redo_vm_clear(record, IH_BLK_VM, oldblk, rlocator);
		if (xlrec->flags & XLIH_NEW_VM_CLEARED)
			redo_vm_clear(record, IH_BLK_NEWVM, newblk, rlocator);
	}

	action = XLogReadBufferForRedo(record, IH_BLK_DATA, &buf);
	if (action == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buf);
		ItemId		lp = PageGetItemId(page, xlrec->old_offnum);
		Size		len;
		char	   *data = XLogRecGetBlockData(record, IH_BLK_DATA, &len);

		memcpy((char *) page + ItemIdGetOffset(lp), data, IHEAP_TUPHDR_SIZE);
		IHeapPageGetOpaque(page)->ihp_flags |= IHP_PRUNABLE;
		if (same)
		{
			if (iheap_page_add_item(page, data + IHEAP_TUPHDR_SIZE, xlrec->new_len,
									xlrec->new_offnum) != xlrec->new_offnum)
				elog(PANIC, "iheap redo: failed to add moved tuple");
		}
		if (xlrec->flags & XLIH_VM_CLEARED)
			PageClearAllVisible(page);
	}
	redo_finish(record, buf, action);

	if (!same)
	{
		Page		page;

		page = redo_data_block(record, IH_BLK_NEWDATA,
							   (xlrec->flags & XLIH_NEW_INIT_PAGE) != 0,
							   &buf, &action);
		if (action == BLK_NEEDS_REDO)
		{
			Size		len;
			char	   *data = XLogRecGetBlockData(record, IH_BLK_NEWDATA, &len);

			if (iheap_page_add_item(page, data, len, xlrec->new_offnum) != xlrec->new_offnum)
				elog(PANIC, "iheap redo: failed to add moved tuple");
			if (xlrec->flags & XLIH_NEW_VM_CLEARED)
				PageClearAllVisible(page);
		}
		redo_finish(record, buf, action);
	}

	redo_undo_block(record, IH_BLK_UNDO);
}

static void
redo_rollback(XLogReaderState *record)
{
	xl_iheap_rollback *xlrec = (xl_iheap_rollback *) XLogRecGetData(record);
	Buffer		buf;
	XLogRedoAction action;

	action = XLogReadBufferForRedo(record, IH_BLK_DATA, &buf);
	if (action == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buf);
		ItemId		lp = PageGetItemId(page, xlrec->offnum);
		Size		len;
		char	   *data = XLogRecGetBlockData(record, IH_BLK_DATA, &len);
		PageHeader	ph = (PageHeader) page;

		switch (xlrec->action)
		{
			case XLIH_RB_KILL:
				ItemIdSetDead(lp);
				IHeapPageGetOpaque(page)->ihp_flags |= IHP_PRUNABLE;
				break;
			case XLIH_RB_HEADER:
				memcpy((char *) page + ItemIdGetOffset(lp), data, IHEAP_TUPHDR_SIZE);
				break;
			case XLIH_RB_TUPLE:
				memcpy((char *) page + xlrec->lp_off, data, xlrec->len);
				ItemIdSetNormal(lp, xlrec->lp_off, xlrec->len);
				if (xlrec->lp_off < ph->pd_upper)
					ph->pd_upper = xlrec->lp_off;
				break;
		}
	}
	redo_finish(record, buf, action);
}

static void
redo_prune(XLogReaderState *record)
{
	xl_iheap_prune *xlrec = (xl_iheap_prune *) XLogRecGetData(record);
	Buffer		buf;
	XLogRedoAction action;

	action = XLogReadBufferForRedo(record, IH_BLK_DATA, &buf);
	if (action == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buf);
		Size		len;
		char	   *data = XLogRecGetBlockData(record, IH_BLK_DATA, &len);
		uint32		reserve[MaxOffsetNumber + 1];
		OffsetNumber off;

		memset(reserve, 0, sizeof(reserve));
		for (int i = 0; i < xlrec->ndead; i++)
		{
			memcpy(&off, data, sizeof(OffsetNumber));
			data += sizeof(OffsetNumber);
			ItemIdSetDead(PageGetItemId(page, off));
		}
		for (int i = 0; i < xlrec->nstub; i++)
		{
			ItemId		lp;
			char	   *tup;

			memcpy(&off, data, sizeof(OffsetNumber));
			data += sizeof(OffsetNumber);
			lp = PageGetItemId(page, off);
			tup = (char *) page + ItemIdGetOffset(lp);
			ih_hdr_set_infomask(tup, ih_hdr_infomask(tup) | IHT_STUB);
			ItemIdSetNormal(lp, ItemIdGetOffset(lp), IHEAP_TUPHDR_SIZE);
		}
		for (int i = 0; i < xlrec->nreserve; i++)
		{
			uint16		rlen;

			memcpy(&off, data, sizeof(OffsetNumber));
			data += sizeof(OffsetNumber);
			memcpy(&rlen, data, sizeof(uint16));
			data += sizeof(uint16);
			reserve[off] = rlen;
		}
		iheap_page_compact(NULL, page, reserve);
		if (xlrec->flags & 1)
			IHeapPageGetOpaque(page)->ihp_flags &= ~IHP_PRUNABLE;
	}
	redo_finish(record, buf, action);
}

static void
redo_visible(XLogReaderState *record)
{
	xl_iheap_visible *xlrec = (xl_iheap_visible *) XLogRecGetData(record);
	RelFileLocator rlocator;
	BlockNumber blk;
	Buffer		buf;
	Buffer		vmbuf = InvalidBuffer;
	XLogRedoAction action;

	XLogRecGetBlockTag(record, IH_BLK_DATA, &rlocator, NULL, &blk);

	action = XLogReadBufferForRedo(record, IH_BLK_DATA, &buf);
	if (action == BLK_NEEDS_REDO)
	{
		PageSetAllVisible(BufferGetPage(buf));
		PageSetLSN(BufferGetPage(buf), record->EndRecPtr);
		MarkBufferDirty(buf);
	}

	if (XLogReadBufferForRedoExtended(record, IH_BLK_VM, RBM_ZERO_ON_ERROR, false,
									  &vmbuf) == BLK_NEEDS_REDO)
	{
		Page		vmpage = BufferGetPage(vmbuf);

		if (PageIsNew(vmpage))
			PageInit(vmpage, BLCKSZ, 0);
		visibilitymap_set(blk, vmbuf, xlrec->vmflags, rlocator);
		PageSetLSN(vmpage, record->EndRecPtr);
	}
	if (BufferIsValid(vmbuf))
		UnlockReleaseBuffer(vmbuf);
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

static void
redo_undo_alloc(XLogReaderState *record)
{
	xl_iheap_undo_alloc *xlrec = (xl_iheap_undo_alloc *) XLogRecGetData(record);
	Buffer		buf;

	redo_meta_block(record, IH_BLK_META);

	buf = XLogInitBufferForRedo(record, IH_BLK_NEWUNDO);
	iheap_undo_init_page(BufferGetPage(buf), xlrec->lpage);
	PageSetLSN(BufferGetPage(buf), record->EndRecPtr);
	MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);
}

static void
redo_discard(XLogReaderState *record)
{
	xl_iheap_discard *xlrec = (xl_iheap_discard *) XLogRecGetData(record);

	if (InHotStandby && TransactionIdIsValid(xlrec->snapshotConflictHorizon))
	{
		RelFileLocator rlocator;

		XLogRecGetBlockTag(record, IH_BLK_META, &rlocator, NULL, NULL);
		ResolveRecoveryConflictWithSnapshot(xlrec->snapshotConflictHorizon,
											false, rlocator);
	}
	redo_meta_block(record, IH_BLK_META);
}

static void
redo_spec_finish(XLogReaderState *record)
{
	xl_iheap_spec_finish *xlrec = (xl_iheap_spec_finish *) XLogRecGetData(record);
	Buffer		buf;
	XLogRedoAction action;

	action = XLogReadBufferForRedo(record, IH_BLK_DATA, &buf);
	if (action == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buf);
		ItemId		lp = PageGetItemId(page, xlrec->offnum);

		if (xlrec->flags & XLIH_SPEC_SUCCEEDED)
		{
			Size		len;
			char	   *hdr = XLogRecGetBlockData(record, IH_BLK_DATA, &len);

			memcpy((char *) page + ItemIdGetOffset(lp), hdr, IHEAP_TUPHDR_SIZE);
		}
		else
		{
			ItemIdSetDead(lp);
			IHeapPageGetOpaque(page)->ihp_flags |= IHP_PRUNABLE;
		}
	}
	redo_finish(record, buf, action);
}

static void
redo_unused(XLogReaderState *record)
{
	xl_iheap_unused *xlrec = (xl_iheap_unused *) XLogRecGetData(record);
	Buffer		buf;
	XLogRedoAction action;

	action = XLogReadBufferForRedo(record, IH_BLK_DATA, &buf);
	if (action == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buf);
		Size		len;
		char	   *data = XLogRecGetBlockData(record, IH_BLK_DATA, &len);

		iheap_page_set_unused(page, (OffsetNumber *) data, xlrec->noffsets);
	}
	redo_finish(record, buf, action);
}

void
iheap_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & XLOG_IHEAP_OPMASK;

	switch (info)
	{
		case XLOG_IHEAP_INSERT:
			redo_insert(record);
			break;
		case XLOG_IHEAP_MULTI_INSERT:
			redo_multi_insert(record);
			break;
		case XLOG_IHEAP_DELETE:
			redo_hdrchange(record, true);
			break;
		case XLOG_IHEAP_LOCK:
			redo_hdrchange(record, false);
			break;
		case XLOG_IHEAP_UPDATE:
			redo_update(record);
			break;
		case XLOG_IHEAP_MOVE:
			redo_move(record);
			break;
		case XLOG_IHEAP_ROLLBACK:
			redo_rollback(record);
			break;
		case XLOG_IHEAP_PRUNE:
			redo_prune(record);
			break;
		case XLOG_IHEAP_VISIBLE:
			redo_visible(record);
			break;
		case XLOG_IHEAP_UNDO_ALLOC:
			redo_undo_alloc(record);
			break;
		case XLOG_IHEAP_DISCARD:
			redo_discard(record);
			break;
		case XLOG_IHEAP_META:
			redo_meta_block(record, IH_BLK_META);
			break;
		case XLOG_IHEAP_SPEC_FINISH:
			redo_spec_finish(record);
			break;
		case XLOG_IHEAP_UNUSED:
			redo_unused(record);
			break;
		case XLOG_IHEAP_UNDO:
			redo_undo_block(record, IH_BLK_UNDO);
			break;
		default:
			elog(PANIC, "iheap_redo: unknown op code %u", info);
	}
}

/* ------------------------------------------------------------------------
 * desc / identify
 * ------------------------------------------------------------------------
 */
const char *
iheap_identify(uint8 info)
{
	switch (info & XLOG_IHEAP_OPMASK)
	{
		case XLOG_IHEAP_INSERT:
			return "INSERT";
		case XLOG_IHEAP_MULTI_INSERT:
			return "MULTI_INSERT";
		case XLOG_IHEAP_DELETE:
			return "DELETE";
		case XLOG_IHEAP_UPDATE:
			return "UPDATE";
		case XLOG_IHEAP_MOVE:
			return "MOVE";
		case XLOG_IHEAP_LOCK:
			return "LOCK";
		case XLOG_IHEAP_ROLLBACK:
			return "ROLLBACK";
		case XLOG_IHEAP_PRUNE:
			return "PRUNE";
		case XLOG_IHEAP_VISIBLE:
			return "VISIBLE";
		case XLOG_IHEAP_UNDO_ALLOC:
			return "UNDO_ALLOC";
		case XLOG_IHEAP_DISCARD:
			return "DISCARD";
		case XLOG_IHEAP_META:
			return "META";
		case XLOG_IHEAP_SPEC_FINISH:
			return "SPEC_FINISH";
		case XLOG_IHEAP_UNUSED:
			return "UNUSED";
		case XLOG_IHEAP_UNDO:
			return "UNDO";
	}
	return NULL;
}

void
iheap_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & XLOG_IHEAP_OPMASK;

	switch (info)
	{
		case XLOG_IHEAP_INSERT:
			appendStringInfo(buf, "off: %u, flags: 0x%02X",
							 ((xl_iheap_insert *) rec)->offnum,
							 ((xl_iheap_insert *) rec)->flags);
			break;
		case XLOG_IHEAP_MULTI_INSERT:
			appendStringInfo(buf, "ntuples: %u, flags: 0x%02X",
							 ((xl_iheap_multi_insert *) rec)->ntuples,
							 ((xl_iheap_multi_insert *) rec)->flags);
			break;
		case XLOG_IHEAP_DELETE:
		case XLOG_IHEAP_LOCK:
			appendStringInfo(buf, "off: %u", ((xl_iheap_hdrchange *) rec)->offnum);
			break;
		case XLOG_IHEAP_UPDATE:
			{
				xl_iheap_update *x = (xl_iheap_update *) rec;

				appendStringInfo(buf, "off: %u, len: %u, prefix: %u, suffix: %u, flags: 0x%02X",
								 x->offnum, x->new_len, x->prefix, x->suffix, x->flags);
				break;
			}
		case XLOG_IHEAP_MOVE:
			{
				xl_iheap_move *x = (xl_iheap_move *) rec;

				appendStringInfo(buf, "old off: %u, new off: %u, len: %u, flags: 0x%02X",
								 x->old_offnum, x->new_offnum, x->new_len, x->flags);
				break;
			}
		case XLOG_IHEAP_ROLLBACK:
			appendStringInfo(buf, "off: %u, action: %u",
							 ((xl_iheap_rollback *) rec)->offnum,
							 ((xl_iheap_rollback *) rec)->action);
			break;
		case XLOG_IHEAP_PRUNE:
			appendStringInfo(buf, "ndead: %u, nstub: %u, nreserve: %u",
							 ((xl_iheap_prune *) rec)->ndead,
							 ((xl_iheap_prune *) rec)->nstub,
							 ((xl_iheap_prune *) rec)->nreserve);
			break;
		case XLOG_IHEAP_VISIBLE:
			appendStringInfo(buf, "vmflags: 0x%02X", ((xl_iheap_visible *) rec)->vmflags);
			break;
		case XLOG_IHEAP_UNDO_ALLOC:
			appendStringInfo(buf, "lpage: " UINT64_FORMAT,
							 ((xl_iheap_undo_alloc *) rec)->lpage);
			break;
		case XLOG_IHEAP_DISCARD:
			appendStringInfo(buf, "snapshotConflictHorizon: %u",
							 ((xl_iheap_discard *) rec)->snapshotConflictHorizon);
			break;
		case XLOG_IHEAP_SPEC_FINISH:
			appendStringInfo(buf, "off: %u, flags: 0x%02X",
							 ((xl_iheap_spec_finish *) rec)->offnum,
							 ((xl_iheap_spec_finish *) rec)->flags);
			break;
		case XLOG_IHEAP_UNUSED:
			appendStringInfo(buf, "noffsets: %u", ((xl_iheap_unused *) rec)->noffsets);
			break;
		default:
			break;
	}
}

/* ------------------------------------------------------------------------
 * Masking for wal_consistency_checking
 * ------------------------------------------------------------------------
 */
void
iheap_mask(char *pagedata, BlockNumber blkno)
{
	Page		page = (Page) pagedata;

	mask_page_lsn_and_checksum(page);
	mask_page_hint_bits(page);
	mask_unused_space(page);

	if (IHeapPageGetType(page) == IHEAP_PAGE_DATA)
	{
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
		PageHeader	ph = (PageHeader) page;
		bool		covered[BLCKSZ];

		IHeapPageGetOpaque(page)->ihp_flags &= ~IHP_PRUNABLE;
		memset(covered, 0, sizeof(covered));
		for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		lp = PageGetItemId(page, off);

			if (ItemIdIsNormal(lp) &&
				ItemIdGetOffset(lp) + ItemIdGetLength(lp) <= BLCKSZ)
			{
				char	   *tup = (char *) page + ItemIdGetOffset(lp);

				ih_hdr_set_infomask(tup, ih_hdr_infomask(tup) & ~IHT_XID_HINTS);
				memset(covered + ItemIdGetOffset(lp), 1, ItemIdGetLength(lp));
			}
		}

		/*
		 * Storage no item points to (left behind by relocation, shrinking,
		 * rollback) is never read and may hold stale hint bits.
		 */
		if (ph->pd_upper <= ph->pd_special && ph->pd_special <= BLCKSZ)
		{
			for (int i = ph->pd_upper; i < ph->pd_special; i++)
			{
				if (!covered[i])
					pagedata[i] = MASK_MARKER;
			}
		}
	}
	else if (IHeapPageGetType(page) == IHEAP_PAGE_UNDO)
		IHeapPageGetOpaque(page)->ihp_flags &= ~IHU_DISCARDED;
}

static const RmgrData iheap_rmgr = {
	.rm_name = "iheap",
	.rm_redo = iheap_redo,
	.rm_desc = iheap_desc,
	.rm_identify = iheap_identify,
	.rm_startup = NULL,
	.rm_cleanup = NULL,
	.rm_mask = iheap_mask,
	.rm_decode = iheap_decode
};

void
iheap_rmgr_register(void)
{
	RegisterCustomRmgr(RM_IHEAP_ID, &iheap_rmgr);
}
