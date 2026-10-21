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
 * Tuple headers in WAL (§21 D42)
 * ------------------------------------------------------------------------
 */

/*
 * The WAL form of the header of a tuple a record writes.  Its undo pointer,
 * if any, is one of the record's own undo records: only its offset goes.
 */
void
iheap_walhdr_put(char *dst, const IHeapHdr *h)
{
	uint16		off = UndoRecPtrIsValid(h->urec) ?
		(uint16) UndoRecPtrGetOffset(h->urec) : 0;

	memcpy(dst, &h->infomask2, sizeof(uint16));
	memcpy(dst + 2, &h->infomask, sizeof(uint16));
	dst[4] = (char) h->hoff;
	memcpy(dst + 5, &off, sizeof(uint16));
}

uint16
iheap_walhdr_urec_off(const char *src)
{
	uint16		off;

	memcpy(&off, src + 5, sizeof(uint16));
	return off;
}

/*
 * The header a WAL header form stands for.  lpage: the logical undo page of
 * the record's undo records (0: the page was reused since, its records
 * discarded, so the header is frozen); xid: the record's.
 */
void
iheap_walhdr_get(const char *src, IHeapHdr *h, uint64 lpage, TransactionId xid)
{
	uint16		off = iheap_walhdr_urec_off(src);

	memcpy(&h->infomask2, src, sizeof(uint16));
	memcpy(&h->infomask, src + 2, sizeof(uint16));
	h->hoff = (uint8) src[4];
	if (off == 0)
	{
		/* a frozen insert: no undo */
		h->urec = InvalidUndoRecPtr;
		xid = FrozenTransactionId;
	}
	else
		h->urec = lpage != 0 ? MakeUndoRecPtr(lpage, off) : InvalidUndoRecPtr;
	h->xid = (h->infomask & IHT_COMPACT) ? FrozenTransactionId : xid;
}

/* the WAL form of a change of an existing item's header to h */
void
iheap_walchange_put(char *dst, const IHeapHdr *h)
{
	uint16		off = UndoRecPtrIsValid(h->urec) ?
		(uint16) UndoRecPtrGetOffset(h->urec) : 0;

	memcpy(dst, &h->infomask, sizeof(uint16));
	memcpy(dst + 2, &off, sizeof(uint16));
}

uint16
iheap_walchange_urec_off(const char *src)
{
	uint16		off;

	memcpy(&off, src + 2, sizeof(uint16));
	return off;
}

/*
 * The header an item with header "old" gets from a header change: lpage as
 * for iheap_walhdr_get(), xid the record's, or InvalidTransactionId to keep
 * the creator (a lock).  A compact header that takes an xid becomes a full
 * one, of a stub.
 */
void
iheap_walchange_get(const char *src, const IHeapHdr *old, IHeapHdr *h,
					uint64 lpage, TransactionId xid)
{
	uint16		off = iheap_walchange_urec_off(src);

	h->infomask2 = old->infomask2;
	memcpy(&h->infomask, src, sizeof(uint16));
	h->hoff = old->hoff;
	if ((old->infomask & IHT_COMPACT) && !(h->infomask & IHT_COMPACT))
		h->hoff += IHEAP_TUPHDR_SIZE - IHEAP_COMPACT_HDRSZ;
	h->urec = (off != 0 && lpage != 0) ? MakeUndoRecPtr(lpage, off) :
		InvalidUndoRecPtr;
	if (h->infomask & IHT_COMPACT)
		h->xid = FrozenTransactionId;
	else if (TransactionIdIsValid(xid))
		h->xid = xid;
	else
		h->xid = old->xid;
}

/* a tuple from its header and body (the null bitmap and the data), in dst */
static uint32
redo_form_tuple(char *dst, const IHeapHdr *h, const char *body, uint32 bodylen)
{
	uint32		hs = ih_hdr_size_of(h);

	ih_hdr_write(dst, h);
	memcpy(dst + hs, body, bodylen);
	return hs + bodylen;
}

/* ------------------------------------------------------------------------
 * Redo routines
 *
 * The undo block of a record is replayed before its data blocks: the tuple
 * headers of those carry offsets on the undo page, whose logical number
 * that replay gives (iheap_undo_redo_block()).  It also means that a
 * standby reader never finds a header pointing at an undo record that is
 * not there yet.
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
	Size		len;
	char	   *data = XLogRecGetBlockData(record, IH_BLK_DATA, &len);
	uint64		lpage;

	XLogRecGetBlockTag(record, IH_BLK_DATA, &rlocator, NULL, &blk);
	if (xlrec->flags & XLIH_VM_CLEARED)
		redo_vm_clear(record, IH_BLK_VM, blk, rlocator);
	lpage = iheap_undo_redo_block(record, IH_BLK_UNDO, data ? 0 : -1,
								  data ? iheap_walhdr_urec_off(data) : 0, NULL);

	page = redo_data_block(record, IH_BLK_DATA, (xlrec->flags & XLIH_INIT_PAGE) != 0,
						   &buf, &action);
	if (action == BLK_NEEDS_REDO)
	{
		PGAlignedBlock tbuf;
		IHeapHdr	h;
		uint32		tlen;

		if (data == NULL || len < SizeOfIHeapWalHdr)
			elog(PANIC, "iheap redo: insert without its tuple");
		iheap_walhdr_get(data, &h, lpage, XLogRecGetXid(record));
		tlen = redo_form_tuple(tbuf.data, &h, data + SizeOfIHeapWalHdr,
							   len - SizeOfIHeapWalHdr);
		if (iheap_page_add_item(page, tbuf.data, tlen, xlrec->offnum) != xlrec->offnum)
			elog(PANIC, "iheap redo: failed to add tuple");
		if (xlrec->flags & XLIH_VM_CLEARED)
			PageClearAllVisible(page);
	}
	redo_finish(record, buf, action);
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
	uint64		lpage;

	XLogRecGetBlockTag(record, IH_BLK_DATA, &rlocator, NULL, &blk);
	if (xlrec->flags & XLIH_VM_CLEARED)
		redo_vm_clear(record, IH_BLK_VM, blk, rlocator);
	lpage = iheap_undo_redo_block(record, IH_BLK_UNDO,
								  xlrec->urec_off != 0 ? 0 : -1,
								  xlrec->urec_off, NULL);

	page = redo_data_block(record, IH_BLK_DATA, (xlrec->flags & XLIH_INIT_PAGE) != 0,
						   &buf, &action);
	if (action == BLK_NEEDS_REDO)
	{
		Size		len;
		char	   *data = XLogRecGetBlockData(record, IH_BLK_DATA, &len);
		char	   *end = data + len;
		IHeapHdr	h;
		TransactionId xid = XLogRecGetXid(record);

		h.urec = InvalidUndoRecPtr;
		if (xlrec->urec_off == 0)
			xid = FrozenTransactionId;
		else if (lpage != 0)
			h.urec = MakeUndoRecPtr(lpage, xlrec->urec_off);
		for (int i = 0; i < xlrec->ntuples; i++)
		{
			PGAlignedBlock tbuf;
			OffsetNumber off;
			uint16		bodylen;
			uint32		tlen;

			if (data + SizeOfIHeapMultiTuple > end)
				elog(PANIC, "iheap redo: truncated multi-insert");
			memcpy(&off, data, sizeof(OffsetNumber));
			memcpy(&bodylen, data + 2, sizeof(uint16));
			memcpy(&h.infomask2, data + 4, sizeof(uint16));
			memcpy(&h.infomask, data + 6, sizeof(uint16));
			h.hoff = (uint8) data[8];
			h.xid = (h.infomask & IHT_COMPACT) ? FrozenTransactionId : xid;
			data += SizeOfIHeapMultiTuple;
			if (data + bodylen > end)
				elog(PANIC, "iheap redo: truncated multi-insert");
			tlen = redo_form_tuple(tbuf.data, &h, data, bodylen);
			if (iheap_page_add_item(page, tbuf.data, tlen, off) != off)
				elog(PANIC, "iheap redo: failed to add tuple");
			data += bodylen;
		}
		if (xlrec->flags & XLIH_VM_CLEARED)
			PageClearAllVisible(page);
	}
	redo_finish(record, buf, action);
}

/*
 * A header change of the item at "off" on page (DELETE, LOCK, the old item
 * of a MOVE).  xid: as for iheap_walchange_get().
 */
static void
redo_apply_change(Page page, OffsetNumber off, const char *change,
				  uint64 lpage, TransactionId xid)
{
	ItemId		lp = PageGetItemId(page, off);
	char	   *tup = (char *) page + ItemIdGetOffset(lp);
	IHeapHdr	old;
	IHeapHdr	h;

	ih_hdr_read(tup, &old);
	iheap_walchange_get(change, &old, &h, lpage, xid);
	ih_hdr_write(tup, &h);
	if ((old.infomask & IHT_COMPACT) && !(h.infomask & (IHT_COMPACT | IHT_STUB)))
		elog(PANIC, "iheap redo: compact header grows outside a stub");
	if ((h.infomask & IHT_STUB) && !(old.infomask & IHT_STUB))
	{
		/*
		 * A row deleted or moved with its body in undo (a compact header, or
		 * a move off a page whose bodies a snapshot holds, §21 D45): a stub
		 */
		if (ItemIdGetLength(lp) < IHEAP_TUPHDR_SIZE)
			elog(PANIC, "iheap redo: stub longer than its item");
		ItemIdSetNormal(lp, ItemIdGetOffset(lp), IHEAP_TUPHDR_SIZE);
		iheap_page_note_room(page, h.xid);
	}
}

/* DELETE and LOCK: change the item's header */
static void
redo_hdrchange(XLogReaderState *record, bool delete)
{
	xl_iheap_hdrchange *xlrec = (xl_iheap_hdrchange *) XLogRecGetData(record);
	RelFileLocator rlocator;
	BlockNumber blk;
	Buffer		buf;
	XLogRedoAction action;
	Size		len;
	char	   *data = XLogRecGetBlockData(record, IH_BLK_DATA, &len);
	uint64		lpage;

	XLogRecGetBlockTag(record, IH_BLK_DATA, &rlocator, NULL, &blk);
	if (xlrec->flags & XLIH_VM_CLEARED)
		redo_vm_clear(record, IH_BLK_VM, blk, rlocator);
	lpage = iheap_undo_redo_block(record, IH_BLK_UNDO, data ? 0 : -1,
								  data ? iheap_walchange_urec_off(data) : 0,
								  NULL);

	action = XLogReadBufferForRedo(record, IH_BLK_DATA, &buf);
	if (action == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buf);

		if (data == NULL || len < SizeOfIHeapWalHdrChange)
			elog(PANIC, "iheap redo: header change without its header");
		redo_apply_change(page, xlrec->offnum, data, lpage,
						  delete ? XLogRecGetXid(record) : InvalidTransactionId);
		if (delete)
			IHeapPageGetOpaque(page)->ihp_flags |= IHP_PRUNABLE;
		if (xlrec->flags & XLIH_VM_CLEARED)
			PageClearAllVisible(page);
	}
	redo_finish(record, buf, action);
}

static void
redo_update(XLogReaderState *record)
{
	xl_iheap_update *xlrec = (xl_iheap_update *) XLogRecGetData(record);
	RelFileLocator rlocator;
	BlockNumber blk;
	Buffer		buf;
	XLogRedoAction action;
	Size		len;
	char	   *data = XLogRecGetBlockData(record, IH_BLK_DATA, &len);
	IHeapUndoDeltaSrc delta;
	uint64		lpage;

	XLogRecGetBlockTag(record, IH_BLK_DATA, &rlocator, NULL, &blk);
	if (xlrec->flags & XLIH_VM_CLEARED)
		redo_vm_clear(record, IH_BLK_VM, blk, rlocator);
	delta.prefix = xlrec->prefix;
	delta.suffix = xlrec->suffix;
	lpage = iheap_undo_redo_block(record, IH_BLK_UNDO, data ? 0 : -1,
								  data ? iheap_walhdr_urec_off(data) : 0, &delta);

	action = XLogReadBufferForRedo(record, IH_BLK_DATA, &buf);
	if (action == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buf);
		ItemId		lp = PageGetItemId(page, xlrec->offnum);
		char	   *old = (char *) page + ItemIdGetOffset(lp);
		uint32		oldlen = ItemIdGetLength(lp);
		uint32		oldhs = ih_hdr_size(old);
		uint32		newmid;
		uint32		newhs;
		IHeapHdr	h;
		PGAlignedBlock tbuf;
		char	   *t = tbuf.data;

		if (data == NULL || len < SizeOfIHeapWalHdr)
			elog(PANIC, "iheap redo: update without its header");
		newmid = len - SizeOfIHeapWalHdr;
		iheap_walhdr_get(data, &h, lpage, XLogRecGetXid(record));
		newhs = ih_hdr_size_of(&h);
		if ((uint32) xlrec->prefix + xlrec->suffix + oldhs > oldlen ||
			newhs + xlrec->prefix + newmid + xlrec->suffix != xlrec->new_len)
			elog(PANIC, "iheap redo: update does not match the page");

		ih_hdr_write(t, &h);
		memcpy(t + newhs, old + oldhs, xlrec->prefix);
		memcpy(t + newhs + xlrec->prefix, data + SizeOfIHeapWalHdr, newmid);
		memcpy(t + newhs + xlrec->prefix + newmid,
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
			if (ItemIdGetOffset(lp) != xlrec->lp_off)
				elog(PANIC, "iheap redo: update diverged (%u vs %u)",
					 ItemIdGetOffset(lp), xlrec->lp_off);
			memcpy(old, t, xlrec->new_len);
			ItemIdSetNormal(lp, ItemIdGetOffset(lp), xlrec->new_len);
		}
		if ((xlrec->flags & XLIH_RELOCATED) || xlrec->new_len < oldlen)
			IHeapPageGetOpaque(page)->ihp_flags |= IHP_PRUNABLE;
		if (xlrec->flags & XLIH_VM_CLEARED)
			PageClearAllVisible(page);
	}
	redo_finish(record, buf, action);
}

/*
 * The new tuple of a MOVE: its WAL form at src, of srclen bytes.  With
 * delta (a move on the same page), the body there is the middle that
 * differs from the body of the old item at old_off, still on the page.
 */
static void
redo_move_add(Page page, OffsetNumber off, const char *src, uint32 srclen,
			  uint64 lpage, TransactionId xid, const xl_iheap_move_delta *delta,
			  OffsetNumber old_off, uint16 new_len)
{
	PGAlignedBlock tbuf;
	IHeapHdr	h;
	uint32		tlen;

	if (srclen < SizeOfIHeapWalHdr)
		elog(PANIC, "iheap redo: move without its tuple");
	iheap_walhdr_get(src, &h, lpage, xid);
	if (delta == NULL)
		tlen = redo_form_tuple(tbuf.data, &h, src + SizeOfIHeapWalHdr,
							   srclen - SizeOfIHeapWalHdr);
	else
	{
		ItemId		lp = PageGetItemId(page, old_off);
		char	   *old = (char *) page + ItemIdGetOffset(lp);
		uint32		oldhs = ih_hdr_size(old);
		uint32		oldbody = ItemIdGetLength(lp) - oldhs;
		uint32		mid = srclen - SizeOfIHeapWalHdr;
		uint32		hs = ih_hdr_size_of(&h);

		if ((uint32) delta->prefix + delta->suffix > oldbody ||
			hs + delta->prefix + mid + delta->suffix != new_len)
			elog(PANIC, "iheap redo: move does not match the page");
		ih_hdr_write(tbuf.data, &h);
		memcpy(tbuf.data + hs, old + oldhs, delta->prefix);
		memcpy(tbuf.data + hs + delta->prefix, src + SizeOfIHeapWalHdr, mid);
		memcpy(tbuf.data + hs + delta->prefix + mid,
			   old + oldhs + oldbody - delta->suffix, delta->suffix);
		tlen = new_len;
	}
	if (iheap_page_add_item(page, tbuf.data, tlen, off) != off)
		elog(PANIC, "iheap redo: failed to add moved tuple");
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
	Size		len;
	char	   *data = XLogRecGetBlockData(record, IH_BLK_DATA, &len);
	Size		newlen = 0;
	char	   *newdata = NULL;
	TransactionId xid = XLogRecGetXid(record);
	uint64		lpage;

	XLogRecGetBlockTag(record, IH_BLK_DATA, &rlocator, NULL, &oldblk);
	if (!same)
	{
		XLogRecGetBlockTag(record, IH_BLK_NEWDATA, NULL, NULL, &newblk);
		newdata = XLogRecGetBlockData(record, IH_BLK_NEWDATA, &newlen);
	}

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

	/* the MOVE record is the first undo record, the new home's INSERT the second */
	if (data != NULL)
		lpage = iheap_undo_redo_block(record, IH_BLK_UNDO, 0,
									  iheap_walchange_urec_off(data), NULL);
	else if (newdata != NULL)
		lpage = iheap_undo_redo_block(record, IH_BLK_UNDO, 1,
									  iheap_walhdr_urec_off(newdata), NULL);
	else
		lpage = iheap_undo_redo_block(record, IH_BLK_UNDO, -1, 0, NULL);

	action = XLogReadBufferForRedo(record, IH_BLK_DATA, &buf);
	if (action == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buf);

		if (data == NULL || len < SizeOfIHeapWalHdrChange)
			elog(PANIC, "iheap redo: move without its header change");
		redo_apply_change(page, xlrec->old_offnum, data, lpage, xid);
		IHeapPageGetOpaque(page)->ihp_flags |= IHP_PRUNABLE;
		if (same)
		{
			xl_iheap_move_delta delta;

			if (xlrec->flags & XLIH_MOVE_DELTA)
				memcpy(&delta, XLogRecGetData(record) + SizeOfIHeapMove,
					   sizeof(delta));
			redo_move_add(page, xlrec->new_offnum, data + SizeOfIHeapWalHdrChange,
						  len - SizeOfIHeapWalHdrChange, lpage, xid,
						  (xlrec->flags & XLIH_MOVE_DELTA) ? &delta : NULL,
						  xlrec->old_offnum, xlrec->new_len);
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
			if (newdata == NULL)
				elog(PANIC, "iheap redo: move without its tuple");
			redo_move_add(page, xlrec->new_offnum, newdata, newlen, lpage, xid,
						  NULL, InvalidOffsetNumber, xlrec->new_len);
			if (xlrec->flags & XLIH_NEW_VM_CLEARED)
				PageClearAllVisible(page);
		}
		redo_finish(record, buf, action);
	}
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
				{
					/* the image of the header, written in its own form */
					IHeapHdr	h;

					ih_hdr_read(data, &h);
					ih_hdr_write((char *) page + ItemIdGetOffset(lp), &h);
					break;
				}
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

	/*
	 * Bodies of deleted and moved rows go: standby queries that may still
	 * see them are cancelled first (§21 D40).
	 */
	if (xlrec->flags & XLIH_PRUNE_CONFLICT)
	{
		TransactionId horizon;

		memcpy(&horizon, XLogRecGetData(record) + SizeOfIHeapPrune,
			   sizeof(TransactionId));
		if (InHotStandby && TransactionIdIsValid(horizon))
		{
			RelFileLocator rlocator;

			XLogRecGetBlockTag(record, IH_BLK_DATA, &rlocator, NULL, NULL);
			ResolveRecoveryConflictWithSnapshot(horizon, false, rlocator);
		}
	}

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
		if (xlrec->flags & XLIH_PRUNE_DONE)
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
			char	   *tup = (char *) page + ItemIdGetOffset(lp);

			ih_hdr_set_infomask(tup, ih_hdr_infomask(tup) & ~IHT_SPECULATIVE);
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
			appendStringInfo(buf, "ntuples: %u, undo offset: %u, flags: 0x%02X",
							 ((xl_iheap_multi_insert *) rec)->ntuples,
							 ((xl_iheap_multi_insert *) rec)->urec_off,
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
				if (x->flags & XLIH_MOVE_DELTA)
				{
					xl_iheap_move_delta d;

					memcpy(&d, rec + SizeOfIHeapMove, sizeof(d));
					appendStringInfo(buf, ", prefix: %u, suffix: %u", d.prefix, d.suffix);
				}
				break;
			}
		case XLOG_IHEAP_ROLLBACK:
			appendStringInfo(buf, "off: %u, action: %u",
							 ((xl_iheap_rollback *) rec)->offnum,
							 ((xl_iheap_rollback *) rec)->action);
			break;
		case XLOG_IHEAP_PRUNE:
			{
				xl_iheap_prune *x = (xl_iheap_prune *) rec;

				appendStringInfo(buf, "ndead: %u, nstub: %u, nreserve: %u",
								 x->ndead, x->nstub, x->nreserve);
				if (x->flags & XLIH_PRUNE_CONFLICT)
				{
					TransactionId horizon;

					memcpy(&horizon, rec + SizeOfIHeapPrune, sizeof(TransactionId));
					appendStringInfo(buf, ", snapshotConflictHorizon: %u", horizon);
				}
				break;
			}
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
		IHeapPageGetOpaque(page)->ihp_room_xid = MASK_MARKER;
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
