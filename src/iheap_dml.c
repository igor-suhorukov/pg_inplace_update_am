/*-------------------------------------------------------------------------
 *
 * iheap_dml.c
 *	  INSERT, DELETE, UPDATE and row locking for iheap.
 *
 * Every change is one WAL record covering the data page, the undo page and,
 * when visibility-map bits are cleared, the VM page.
 *
 * UPDATE is done in place when no index-covered column changed (rule (b):
 * core indexes cannot cope with two keys for one TID), no AFTER trigger needs
 * OLD by TID (rule 3), and the new tuple fits on the page.  Otherwise the row
 * moves to a new TID; the old item keeps its body until every snapshot sees
 * the move, unless the row leaves a page whose bodies a snapshot holds.  A
 * move to another page keeps the old page locked while it locks the new one
 * when the block order allows, so that it needs no LOCK record.
 *
 * Row locks are LOCK undo records listing the lockers that are still
 * running.  A modification carries non-conflicting lockers (KEY SHARE versus
 * a non-key update) into its own record.
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/hio.h"
#include "access/parallel.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/toast_helper.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_trigger.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/timestamp.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "iheap.h"
#include "iheap_xlog.h"

/* heavyweight lock modes of tuple lock modes (as heapam.c) */
static const LOCKMODE ih_hwlock[MaxLockTupleMode + 1] = {
	[LockTupleKeyShare] = AccessShareLock,
	[LockTupleShare] = RowShareLock,
	[LockTupleNoKeyExclusive] = ExclusiveLock,
	[LockTupleExclusive] = AccessExclusiveLock
};

static inline bool
ih_modes_conflict(LockTupleMode a, LockTupleMode b)
{
	return DoLockModesConflict(ih_hwlock[a], ih_hwlock[b]);
}

/* discard undo at a safe point (no buffer locks held) when we allocated */
static void
ih_maybe_discard(Relation rel)
{
	if (iheap_undo_take_alloc_count(rel) > 0)
		(void) iheap_discard(rel, false);
}

/* ------------------------------------------------------------------------
 * Visibility map handling
 * ------------------------------------------------------------------------
 */
typedef struct IHVmState
{
	Buffer		vmbuf;
	bool		locked;
	bool		borrowed;		/* vmbuf is locked through the other state */
	bool		cleared;
} IHVmState;

#define IHVM_INIT { InvalidBuffer, false, false, false }

/* before the critical section; buffer is exclusively locked */
static void
ih_vm_prepare(Relation rel, Buffer buffer, IHVmState *vm)
{
	if (PageIsAllVisible(BufferGetPage(buffer)))
	{
		Assert(BufferIsValid(vm->vmbuf));
		LockBuffer(vm->vmbuf, BUFFER_LOCK_EXCLUSIVE);
		vm->locked = true;
	}
}

/*
 * The same for the two pages of a row move, like heap_update(): when both
 * map to one VM page it is locked once (newvm borrows the lock), otherwise
 * the VM pages are locked in block order.
 */
static void
ih_vm_prepare_pair(Relation rel, Buffer buffer, IHVmState *vm,
				   Buffer newbuf, IHVmState *newvm)
{
	bool		need_old = PageIsAllVisible(BufferGetPage(buffer));
	bool		need_new = PageIsAllVisible(BufferGetPage(newbuf));

	if (need_old && need_new && vm->vmbuf == newvm->vmbuf)
	{
		Assert(BufferIsValid(vm->vmbuf));
		LockBuffer(vm->vmbuf, BUFFER_LOCK_EXCLUSIVE);
		vm->locked = true;
		newvm->borrowed = true;
	}
	else if (need_old && need_new &&
			 BufferGetBlockNumber(newvm->vmbuf) < BufferGetBlockNumber(vm->vmbuf))
	{
		ih_vm_prepare(rel, newbuf, newvm);
		ih_vm_prepare(rel, buffer, vm);
	}
	else
	{
		ih_vm_prepare(rel, buffer, vm);
		ih_vm_prepare(rel, newbuf, newvm);
	}
}

/* inside the critical section */
static void
ih_vm_clear(Relation rel, Buffer buffer, IHVmState *vm)
{
	Page		page = BufferGetPage(buffer);

	if ((vm->locked || vm->borrowed) && PageIsAllVisible(page))
	{
		if (visibilitymap_clear(rel, BufferGetBlockNumber(buffer), vm->vmbuf,
								VISIBILITYMAP_VALID_BITS))
			vm->cleared = true;
		PageClearAllVisible(page);
	}
}

static void
ih_vm_finish(IHVmState *vm)
{
	if (vm->locked)
		LockBuffer(vm->vmbuf, BUFFER_LOCK_UNLOCK);
	vm->locked = false;
	vm->borrowed = false;
	if (BufferIsValid(vm->vmbuf))
		ReleaseBuffer(vm->vmbuf);
	vm->vmbuf = InvalidBuffer;
}

/*
 * Lock buffer exclusively, making sure the VM page is pinned if the page is
 * all-visible (pinning may do I/O, so it is done without the lock).
 */
static void
ih_lock_with_vm(Relation rel, Buffer buffer, IHVmState *vm)
{
	BlockNumber blk = BufferGetBlockNumber(buffer);

	for (;;)
	{
		if (PageIsAllVisible(BufferGetPage(buffer)) && !BufferIsValid(vm->vmbuf))
			visibilitymap_pin(rel, blk, &vm->vmbuf);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		if (!PageIsAllVisible(BufferGetPage(buffer)) || BufferIsValid(vm->vmbuf))
			return;
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	}
}

/* ------------------------------------------------------------------------
 * Finding a page for a new tuple
 * ------------------------------------------------------------------------
 */

static bool
ih_block_in_extent(Relation rel, BlockNumber blk)
{
	Buffer		metabuf;
	bool		result;

	metabuf = ReadBufferExtended(rel, MAIN_FORKNUM, IHEAP_METAPAGE_BLKNO,
								 RBM_NORMAL, NULL);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	result = iheap_meta_block_in_extent(IHeapPageGetMeta(BufferGetPage(metabuf)), blk);
	UnlockReleaseBuffer(metabuf);
	return result;
}

/* lock two buffers in block order (other may be InvalidBuffer) */
static void
ih_lock_pair(Buffer buffer, Buffer other)
{
	if (!BufferIsValid(other))
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	else if (BufferGetBlockNumber(buffer) < BufferGetBlockNumber(other))
	{
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		LockBuffer(other, BUFFER_LOCK_EXCLUSIVE);
	}
	else
	{
		LockBuffer(other, BUFFER_LOCK_EXCLUSIVE);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	}
}

static void
ih_unlock_pair(Buffer buffer, Buffer other)
{
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	if (BufferIsValid(other))
		LockBuffer(other, BUFFER_LOCK_UNLOCK);
}

/*
 * A data page that is in shared buffers, pinned; InvalidBuffer if it would
 * have to be read (a prefetch is started then).
 */
static Buffer
ih_pin_if_cached(Relation rel, BlockNumber blk)
{
	PrefetchBufferResult pr = PrefetchBuffer(rel, MAIN_FORKNUM, blk);

	if (BufferIsValid(pr.recent_buffer) &&
		ReadRecentBuffer(rel->rd_locator, MAIN_FORKNUM, blk, pr.recent_buffer))
		return pr.recent_buffer;
	return InvalidBuffer;
}

/*
 * Return a pinned, exclusively locked data page with room for a tuple of
 * "len" bytes.  If "otherbuf" is given (pinned, different page), it is
 * locked too: in block order if it comes unlocked.  If it comes locked
 * ("other_locked"), it stays locked throughout: a page after it is locked as
 * the block order allows, one before it only if its lock is free at once, and
 * a page is not read from disk with otherbuf locked; when that leaves no
 * page, InvalidBuffer is returned, and the caller lets go of otherbuf and
 * asks again.  *page_init is set when the page was freshly initialized in
 * memory (the caller's WAL record must use REGBUF_WILL_INIT).
 */
static Buffer
ih_get_buffer(Relation rel, Size len, BulkInsertStateData *bistate,
			  Buffer otherbuf, bool other_locked, bool *page_init)
{
	Size		need = len + sizeof(ItemIdData) + MAXIMUM_ALIGNOF;
	Size		saveFreeSpace = RelationGetTargetPageFreeSpace(rel, HEAP_DEFAULT_FILLFACTOR);
	Size		nearly_empty = (BLCKSZ - SizeOfPageHeaderData - IHEAP_SPECIAL_SIZE) -
		(IHEAP_MAX_ITEMS / 8 * sizeof(ItemIdData));
	Size		want_space;
	BufferAccessStrategy strategy = bistate ? bistate->strategy : NULL;
	BlockNumber otherblk = BufferIsValid(otherbuf) ? BufferGetBlockNumber(otherbuf) : InvalidBlockNumber;
	BlockNumber target;
	Buffer		buffer;
	int			tries = 0;

	*page_init = false;

	if (len > IHEAP_MAX_TUPLE_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("row is too big: size %zu, maximum size %zu",
						len, (Size) IHEAP_MAX_TUPLE_SIZE)));

	/*
	 * As heap: when fillfactor leaves no room for the tuple on a partly
	 * filled page, it goes to a (nearly) empty one, rather than ignoring
	 * fillfactor.
	 */
	if (need + saveFreeSpace > nearly_empty)
		want_space = Max(need, nearly_empty);
	else
		want_space = need + saveFreeSpace;

	target = RelationGetTargetBlock(rel);
	if (target == InvalidBlockNumber)
		target = GetPageWithFreeSpace(rel, want_space);

	while (target != InvalidBlockNumber && tries++ < 100)
	{
		Page		page;
		uint16		type;
		Size		freespace = 0;

		if (target == IHEAP_METAPAGE_BLKNO || target == otherblk ||
			target >= RelationGetNumberOfBlocks(rel))
		{
			target = RecordAndGetPageWithFreeSpace(rel, target, 0, want_space);
			continue;
		}

		if (!other_locked)
		{
			buffer = ReadBufferExtended(rel, MAIN_FORKNUM, target, RBM_NORMAL,
										strategy);
			ih_lock_pair(buffer, otherbuf);
		}
		else
		{
			/* waiting for an earlier page could deadlock */
			buffer = ih_pin_if_cached(rel, target);
			if (!BufferIsValid(buffer))
				return InvalidBuffer;
			if (target > otherblk)
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			else if (!ConditionalLockBuffer(buffer))
			{
				ReleaseBuffer(buffer);
				return InvalidBuffer;
			}
		}
		page = BufferGetPage(buffer);
		type = IHeapPageGetType(page);

		if (type == IHEAP_PAGE_DATA)
		{
			freespace = iheap_page_free_space(page);
			if (freespace < want_space &&
				(IHeapPageGetOpaque(page)->ihp_flags & IHP_PRUNABLE))
			{
				(void) iheap_page_prune_opt(rel, buffer);
				freespace = iheap_page_free_space(page);
			}
			if (freespace >= want_space)
			{
				RelationSetTargetBlock(rel, target);
				return buffer;
			}
		}
		else if ((PageIsNew(page) || type == IHEAP_PAGE_UNDO) &&
				 !ih_block_in_extent(rel, target))
		{
			/* a page released by undo, or never initialized */
			iheap_init_data_page(page);
			*page_init = true;
			RelationSetTargetBlock(rel, target);
			return buffer;
		}

		if (other_locked)
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		else
			ih_unlock_pair(buffer, otherbuf);
		ReleaseBuffer(buffer);
		target = RecordAndGetPageWithFreeSpace(rel, target, freespace,
											   want_space);
	}

	/* extend the relation */
	if (BufferIsValid(otherbuf) && !other_locked)
		LockBuffer(otherbuf, BUFFER_LOCK_EXCLUSIVE);
	buffer = ExtendBufferedRel(BMR_REL(rel), MAIN_FORKNUM, strategy,
							   EB_LOCK_FIRST);
	iheap_init_data_page(BufferGetPage(buffer));
	*page_init = true;
	RelationSetTargetBlock(rel, BufferGetBlockNumber(buffer));
	return buffer;
}

Buffer
iheap_get_buffer_for_tuple(Relation rel, Size len,
						   BulkInsertStateData *bistate, Buffer otherbuf,
						   bool *page_init)
{
	return ih_get_buffer(rel, len, bistate, otherbuf, false, page_init);
}

/*
 * How many rows written frozen may have a compact header on one page, -1 for
 * any number.  A later in-place update gives such a row a full header again,
 * 4 bytes more: "auto" lets as many be compact as the space the table's
 * fillfactor keeps free on a page covers.
 */
int
iheap_compact_quota(Relation rel)
{
	switch (iheap_compact_headers)
	{
		case IHEAP_COMPACT_OFF:
			return 0;
		case IHEAP_COMPACT_ALWAYS:
			return -1;
		default:
			return (int) (RelationGetTargetPageFreeSpace(rel, HEAP_DEFAULT_FILLFACTOR) /
						  (IHEAP_TUPHDR_SIZE - IHEAP_COMPACT_HDRSZ));
	}
}

/* rows with a compact header on a page */
int
iheap_page_compact_count(Page page)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	int			n = 0;

	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		lp = PageGetItemId(page, off);

		if (ItemIdIsNormal(lp) &&
			(ih_hdr_infomask((char *) page + ItemIdGetOffset(lp)) & IHT_COMPACT))
			n++;
	}
	return n;
}

/* how many more rows written frozen may get a compact header on page */
static int
ih_compact_budget(Relation rel, Page page)
{
	int			quota = iheap_compact_quota(rel);

	if (quota < 0)
		return PG_INT32_MAX;
	if (quota == 0)
		return 0;
	return Max(quota - iheap_page_compact_count(page), 0);
}

/* ------------------------------------------------------------------------
 * INSERT
 * ------------------------------------------------------------------------
 */

static void
ih_copy_slot_values(TupleTableSlot *slot, int natts, Datum **values,
					bool **isnull)
{
	slot_getallattrs(slot);
	*values = palloc(Max(natts, 1) * sizeof(Datum));
	*isnull = palloc(Max(natts, 1) * sizeof(bool));
	memcpy(*values, slot->tts_values, natts * sizeof(Datum));
	memcpy(*isnull, slot->tts_isnull, natts * sizeof(bool));
}

static void
ih_insert(Relation rel, TupleTableSlot *slot, CommandId cid, uint32 options,
		  BulkInsertStateData *bistate, bool speculative, uint32 token)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Datum	   *values;
	bool	   *isnull;
	IHeapToastState *ts = NULL;
	char	   *tup;
	char	   *mem;
	uint32		len;
	Buffer		buffer;
	Page		page;
	bool		page_init;
	IHeapUndoReservation res;
	IHVmState	vm = IHVM_INIT;
	bool		frozen = (options & TABLE_INSERT_FROZEN) != 0 && !speculative;
	TransactionId xid;
	OffsetNumber offnum;
	UndoRecPtr	urp = InvalidUndoRecPtr;
	IHeapUndoRecHeader uh;
	IHeapHdr	h;
	uint32		payload_len = speculative ? sizeof(uint32) : 0;
	bool		logical = iheap_logically_logged(rel,
												 (options & TABLE_INSERT_NO_LOGICAL) != 0);
	IHeapLogicalPart logpart = {0};

	/* as heap: the leader of a parallel CREATE TABLE AS may insert */
	if (IsParallelWorker())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot insert tuples in a parallel worker")));

	ih_maybe_discard(rel);

	ih_copy_slot_values(slot, desc->natts, &values, &isnull);
	if (iheap_tuple_needs_toast(rel, desc, values, isnull, false))
	{
		ts = palloc(sizeof(IHeapToastState));
		iheap_toast_begin(ts, rel, desc, values, isnull, NULL, NULL, options);
	}
	tup = ih_form_tuple(desc, values, isnull, &len, &mem);

	xid = GetCurrentTransactionId();

	CheckForSerializableConflictIn(rel, NULL, InvalidBlockNumber);

	buffer = iheap_get_buffer_for_tuple(rel, len, bistate, InvalidBuffer,
										&page_init);
	page = BufferGetPage(buffer);

	/* a row written frozen may take a compact header */
	if (frozen && iheap_compact_headers != IHEAP_COMPACT_OFF &&
		ih_tuple_can_compact(tup, len) && ih_compact_budget(rel, page) > 0)
		len = ih_tuple_compact(tup, len);
	if (PageIsAllVisible(page))
	{
		/* pin VM without holding the lock, then recheck */
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ih_lock_with_vm(rel, buffer, &vm);
		if (iheap_page_free_space(page) < len + sizeof(ItemIdData) + MAXIMUM_ALIGNOF)
		{
			/* lost the space while unlocked: start over */
			UnlockReleaseBuffer(buffer);
			ih_vm_finish(&vm);
			RelationSetTargetBlock(rel, InvalidBlockNumber);
			pfree(mem);
			if (ts)
				iheap_toast_end(ts);
			pfree(values);
			pfree(isnull);
			ih_insert(rel, slot, cid, options, bistate, speculative, token);
			return;
		}
	}

	memset(&res, 0, sizeof(res));
	res.buffer = InvalidBuffer;
	if (!frozen)
		iheap_undo_reserve(rel, IHEAP_UNDOREC_HDRSZ + payload_len, &res);
	if (logical)
		iheap_logical_prepare(&logpart, rel, true, NULL, NULL, false);

	offnum = iheap_page_choose_offnum(page);
	ih_vm_prepare(rel, buffer, &vm);

	memset(&uh, 0, sizeof(uh));
	uh.ur_type = IHU_INSERT;
	uh.ur_flags = speculative ? IHUF_SPEC : 0;
	uh.ur_xid = xid;
	uh.ur_cid = cid;
	uh.ur_blkno = BufferGetBlockNumber(buffer);
	uh.ur_offnum = offnum;

	START_CRIT_SECTION();

	if (!frozen)
	{
		uh.ur_xact_prev = iheap_xact_last_urp(rel);
		urp = iheap_undo_write(rel, &res, &uh, (const char *) &token,
							   payload_len);
	}

	ih_hdr_read(tup, &h);
	h.infomask &= IHT_DATA_FLAGS | IHT_COMPACT;
	if (speculative)
		h.infomask |= IHT_SPECULATIVE;
	if (frozen)
	{
		h.xid = FrozenTransactionId;
		h.urec = InvalidUndoRecPtr;
	}
	else
	{
		h.xid = xid;
		h.urec = urp;
	}
	ih_hdr_write(tup, &h);

	if (iheap_page_add_item(page, tup, len, offnum) != offnum)
		elog(PANIC, "iheap: failed to add tuple to page");

	ih_vm_clear(rel, buffer, &vm);
	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(rel))
	{
		xl_iheap_insert xlrec;
		XLogRecPtr	recptr;
		char		walhdr[SizeOfIHeapWalHdr];
		uint32		hs = ih_hdr_size(tup);

		xlrec.offnum = offnum;
		xlrec.flags = (page_init ? XLIH_INIT_PAGE : 0) |
			(vm.cleared ? XLIH_VM_CLEARED : 0) |
			(logical ? XLIH_LOGICAL : 0);

		Assert(frozen || UndoRecPtrGetLPage(urp) == res.lpage);
		iheap_walhdr_put(walhdr, &h);
		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfIHeapInsert);
		if (logical)
			XLogRegisterData(logpart.data, logpart.len);
		/* logical decoding reads the tuple even with a full-page image */
		XLogRegisterBuffer(IH_BLK_DATA, buffer,
						   REGBUF_STANDARD | (page_init ? REGBUF_WILL_INIT : 0) |
						   (logical ? REGBUF_KEEP_DATA : 0));
		XLogRegisterBufData(IH_BLK_DATA, walhdr, SizeOfIHeapWalHdr);
		XLogRegisterBufData(IH_BLK_DATA, tup + hs, len - hs);
		if (!frozen)
			iheap_undo_register_wal(&res, IH_BLK_UNDO,
									BufferGetBlockNumber(buffer), false);
		if (vm.cleared)
			XLogRegisterBuffer(IH_BLK_VM, vm.vmbuf, 0);
		recptr = XLogInsert(RM_IHEAP_ID, XLOG_IHEAP_INSERT);
		PageSetLSN(page, recptr);
		if (!frozen)
			iheap_undo_set_lsn(&res, recptr);
		if (vm.cleared)
			PageSetLSN(BufferGetPage(vm.vmbuf), recptr);
	}

	END_CRIT_SECTION();

	if (!frozen)
		iheap_xact_register_write(rel, urp, uh.ur_len);

	ItemPointerSet(&slot->tts_tid, BufferGetBlockNumber(buffer), offnum);

	ih_vm_finish(&vm);
	if (!frozen)
		iheap_undo_release(&res);
	UnlockReleaseBuffer(buffer);
	iheap_logical_free(&logpart);

	if (ts)
		iheap_toast_end(ts);

	slot->tts_tableOid = RelationGetRelid(rel);
	if (TTS_IS_IHEAPTUPLE(slot))
	{
		((IHeapTupleTableSlot *) slot)->xmin = frozen ? FrozenTransactionId : xid;
		((IHeapTupleTableSlot *) slot)->cmin = cid;
	}

	pgstat_count_heap_insert(rel, 1);

	pfree(mem);
	pfree(values);
	pfree(isnull);
}

void
iheap_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid,
				   uint32 options, BulkInsertStateData *bistate)
{
	ih_insert(rel, slot, cid, options, bistate, false, 0);
}

void
iheap_tuple_insert_speculative(Relation rel, TupleTableSlot *slot,
							   CommandId cid, uint32 options,
							   BulkInsertStateData *bistate, uint32 specToken)
{
	ih_insert(rel, slot, cid, options, bistate, true, specToken);
}

void
iheap_tuple_complete_speculative(Relation rel, TupleTableSlot *slot,
								 uint32 specToken, bool succeeded)
{
	ItemPointer tid = &slot->tts_tid;
	Buffer		buffer;
	Page		page;
	ItemId		lp;
	char	   *tup;
	IHeapHdr	h;
	char	   *oldcopy = NULL;
	char	   *oldmem = NULL;

	buffer = ReadBufferExtended(rel, MAIN_FORKNUM, ItemPointerGetBlockNumber(tid),
								RBM_NORMAL, NULL);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buffer);
	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
	if (!ItemIdIsNormal(lp))
	{
		UnlockReleaseBuffer(buffer);
		return;
	}
	tup = (char *) page + ItemIdGetOffset(lp);
	ih_hdr_read(tup, &h);
	if ((h.infomask & IHT_SPECULATIVE) == 0)
	{
		UnlockReleaseBuffer(buffer);
		return;
	}

	if (!succeeded && (h.infomask & IHT_HASEXTERNAL))
		oldcopy = ih_copy_tuple_aligned(CurrentMemoryContext, tup,
										ItemIdGetLength(lp), &oldmem);

	START_CRIT_SECTION();
	if (succeeded)
	{
		h.infomask &= ~IHT_SPECULATIVE;
		ih_hdr_write(tup, &h);
	}
	else
	{
		/* super-delete: the row never existed for anybody else */
		ItemIdSetDead(lp);
		IHeapPageGetOpaque(page)->ihp_flags |= IHP_PRUNABLE;
	}
	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(rel))
	{
		xl_iheap_spec_finish xlrec;
		XLogRecPtr	recptr;

		xlrec.offnum = ItemPointerGetOffsetNumber(tid);
		xlrec.flags = (succeeded ? XLIH_SPEC_SUCCEEDED : 0) |
			(iheap_logically_logged(rel, false) ? XLIH_SPEC_LOGICAL : 0);
		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfIHeapSpecFinish);
		XLogRegisterBuffer(IH_BLK_DATA, buffer, REGBUF_STANDARD);
		recptr = XLogInsert(RM_IHEAP_ID, XLOG_IHEAP_SPEC_FINISH);
		PageSetLSN(page, recptr);
	}
	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);

	if (oldcopy)
	{
		iheap_toast_delete_values(rel, RelationGetDescr(rel), oldcopy, true);
		pfree(oldmem);
	}
}

/*
 * multi_insert (COPY): one MULTI_INSERT undo record and one WAL record per
 * page filled.  As heap's, it keeps the free space fillfactor asks for on
 * each page, but always puts one row on the page it got for it.  Rows of a
 * frozen load may take compact headers.
 */
void
iheap_multi_insert(Relation rel, TupleTableSlot **slots, int nslots,
				   CommandId cid, uint32 options, BulkInsertStateData *bistate)
{
	TupleDesc	desc = RelationGetDescr(rel);
	bool		frozen = (options & TABLE_INSERT_FROZEN) != 0;
	TransactionId xid;
	char	  **tuples = palloc(nslots * sizeof(char *));
	char	  **mems = palloc(nslots * sizeof(char *));
	uint32	   *lens = palloc(nslots * sizeof(uint32));
	IHeapToastState **toasts = palloc0(nslots * sizeof(IHeapToastState *));
	char	   *walbuf = palloc(BLCKSZ * 2);
	int			done = 0;
	bool		logical = iheap_logically_logged(rel,
												 (options & TABLE_INSERT_NO_LOGICAL) != 0);
	IHeapLogicalPart logpart = {0};
	Size		saveFreeSpace = RelationGetTargetPageFreeSpace(rel, HEAP_DEFAULT_FILLFACTOR);

	/* as heap: the leader of a parallel CREATE TABLE AS may insert */
	if (IsParallelWorker())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot insert tuples in a parallel worker")));

	ih_maybe_discard(rel);

	for (int i = 0; i < nslots; i++)
	{
		Datum	   *values;
		bool	   *isnull;

		ih_copy_slot_values(slots[i], desc->natts, &values, &isnull);
		if (iheap_tuple_needs_toast(rel, desc, values, isnull, false))
		{
			toasts[i] = palloc(sizeof(IHeapToastState));
			iheap_toast_begin(toasts[i], rel, desc, values, isnull, NULL, NULL,
							  options);
		}
		tuples[i] = ih_form_tuple(desc, values, isnull, &lens[i], &mems[i]);
		if (toasts[i])
		{
			iheap_toast_end(toasts[i]);
			pfree(toasts[i]);
		}
		pfree(values);
		pfree(isnull);
	}

	xid = GetCurrentTransactionId();
	CheckForSerializableConflictIn(rel, NULL, InvalidBlockNumber);
	if (logical)
		iheap_logical_prepare(&logpart, rel, true, NULL, NULL, false);

	while (done < nslots)
	{
		Buffer		buffer;
		Page		page;
		bool		page_init;
		IHVmState	vm = IHVM_INIT;
		IHeapUndoReservation res;
		int			n = 0;
		Size		used = 0;
		Size		freespace;
		OffsetNumber offs[MaxOffsetNumber];
		UndoRecPtr	urp = InvalidUndoRecPtr;
		IHeapUndoRecHeader uh;
		char		payload[sizeof(uint16) + MaxOffsetNumber * sizeof(OffsetNumber)];
		uint32		payload_len;
		char	   *wp;
		int			maxitems;
		int			budget;

		buffer = iheap_get_buffer_for_tuple(rel, lens[done], bistate,
											InvalidBuffer, &page_init);
		page = BufferGetPage(buffer);
		if (PageIsAllVisible(page))
		{
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			ih_lock_with_vm(rel, buffer, &vm);
		}

		/* how many of the remaining tuples fit */
		freespace = (Size) ((PageHeader) page)->pd_upper - ((PageHeader) page)->pd_lower;
		maxitems = IHEAP_MAX_ITEMS - PageGetMaxOffsetNumber(page);
		budget = (frozen && iheap_compact_headers != IHEAP_COMPACT_OFF) ?
			ih_compact_budget(rel, page) : 0;
		while (done + n < nslots && n < maxitems)
		{
			uint32		tlen = lens[done + n];
			bool		compact = budget > 0 &&
				ih_tuple_can_compact(tuples[done + n], tlen);
			Size		need;

			if (compact)
				tlen -= IHEAP_TUPHDR_SIZE - IHEAP_COMPACT_HDRSZ;
			need = tlen + sizeof(ItemIdData) + MAXIMUM_ALIGNOF;
			if (used + need > freespace ||
				(n > 0 && used + need + saveFreeSpace > freespace))
				break;
			if (compact)
			{
				lens[done + n] = ih_tuple_compact(tuples[done + n], lens[done + n]);
				budget--;
			}
			used += need;
			n++;
		}
		if (n == 0)
		{
			/* the free space was taken under us; try again */
			ih_vm_finish(&vm);
			UnlockReleaseBuffer(buffer);
			RelationSetTargetBlock(rel, InvalidBlockNumber);
			continue;
		}

		payload_len = sizeof(uint16) + n * sizeof(OffsetNumber);
		memset(&res, 0, sizeof(res));
		res.buffer = InvalidBuffer;
		if (!frozen)
			iheap_undo_reserve(rel, IHEAP_UNDOREC_HDRSZ + payload_len, &res);
		ih_vm_prepare(rel, buffer, &vm);

		START_CRIT_SECTION();

		if (!frozen)
			urp = MakeUndoRecPtr(res.lpage, res.offset);

		wp = walbuf;
		for (int k = 0; k < n; k++)
		{
			IHeapHdr	h;
			char	   *t = tuples[done + k];
			uint32		hs = ih_hdr_size(t);
			uint16		bodylen = (uint16) (lens[done + k] - hs);

			ih_hdr_read(t, &h);
			h.infomask &= IHT_DATA_FLAGS | IHT_COMPACT;
			h.xid = frozen ? FrozenTransactionId : xid;
			h.urec = frozen ? InvalidUndoRecPtr : urp;
			ih_hdr_write(t, &h);
			offs[k] = iheap_page_add_item(page, t, lens[done + k], InvalidOffsetNumber);
			if (!OffsetNumberIsValid(offs[k]))
				elog(PANIC, "iheap: failed to add tuple during multi-insert");
			/* offnum, body length, the header without xid and undo pointer */
			memcpy(wp, &offs[k], sizeof(OffsetNumber));
			memcpy(wp + 2, &bodylen, sizeof(uint16));
			memcpy(wp + 4, &h.infomask2, sizeof(uint16));
			memcpy(wp + 6, &h.infomask, sizeof(uint16));
			wp[8] = (char) h.hoff;
			wp += SizeOfIHeapMultiTuple;
			memcpy(wp, t + hs, bodylen);
			wp += bodylen;
		}

		if (!frozen)
		{
			uint16		n16 = (uint16) n;
			UndoRecPtr	got;

			memcpy(payload, &n16, sizeof(uint16));
			memcpy(payload + sizeof(uint16), offs, n * sizeof(OffsetNumber));
			memset(&uh, 0, sizeof(uh));
			uh.ur_type = IHU_MULTI_INSERT;
			uh.ur_xid = xid;
			uh.ur_cid = cid;
			uh.ur_blkno = BufferGetBlockNumber(buffer);
			uh.ur_offnum = InvalidOffsetNumber;
			uh.ur_xact_prev = iheap_xact_last_urp(rel);
			got = iheap_undo_write(rel, &res, &uh, payload, payload_len);
			if (got != urp)
				elog(PANIC, "iheap: unexpected undo position");
		}

		ih_vm_clear(rel, buffer, &vm);
		MarkBufferDirty(buffer);

		if (RelationNeedsWAL(rel))
		{
			xl_iheap_multi_insert xlrec;
			XLogRecPtr	recptr;

			xlrec.ntuples = n;
			xlrec.urec_off = frozen ? 0 : (uint16) UndoRecPtrGetOffset(urp);
			xlrec.flags = (page_init ? XLIH_INIT_PAGE : 0) |
				(vm.cleared ? XLIH_VM_CLEARED : 0) |
				(logical ? XLIH_LOGICAL : 0) |
				(done + n == nslots ? XLIH_LAST_IN_MULTI : 0);
			XLogBeginInsert();
			XLogRegisterData(&xlrec, SizeOfIHeapMultiInsert);
			if (logical)
				XLogRegisterData(logpart.data, logpart.len);
			XLogRegisterBuffer(IH_BLK_DATA, buffer,
							   REGBUF_STANDARD | (page_init ? REGBUF_WILL_INIT : 0) |
							   (logical ? REGBUF_KEEP_DATA : 0));
			XLogRegisterBufData(IH_BLK_DATA, walbuf, wp - walbuf);
			if (!frozen)
				iheap_undo_register_wal(&res, IH_BLK_UNDO,
										BufferGetBlockNumber(buffer), false);
			if (vm.cleared)
				XLogRegisterBuffer(IH_BLK_VM, vm.vmbuf, 0);
			recptr = XLogInsert(RM_IHEAP_ID, XLOG_IHEAP_MULTI_INSERT);
			PageSetLSN(page, recptr);
			if (!frozen)
				iheap_undo_set_lsn(&res, recptr);
			if (vm.cleared)
				PageSetLSN(BufferGetPage(vm.vmbuf), recptr);
		}

		END_CRIT_SECTION();

		if (!frozen)
		{
			iheap_xact_register_write(rel, urp, IHEAP_UNDOREC_HDRSZ + payload_len);
			iheap_undo_release(&res);
		}
		ih_vm_finish(&vm);

		for (int k = 0; k < n; k++)
		{
			TupleTableSlot *s = slots[done + k];

			ItemPointerSet(&s->tts_tid, BufferGetBlockNumber(buffer), offs[k]);
			s->tts_tableOid = RelationGetRelid(rel);
			if (TTS_IS_IHEAPTUPLE(s))
			{
				((IHeapTupleTableSlot *) s)->xmin = frozen ? FrozenTransactionId : xid;
				((IHeapTupleTableSlot *) s)->cmin = cid;
			}
		}
		UnlockReleaseBuffer(buffer);
		done += n;
	}

	pgstat_count_heap_insert(rel, nslots);
	iheap_logical_free(&logpart);

	for (int i = 0; i < nslots; i++)
		pfree(mems[i]);
	pfree(tuples);
	pfree(mems);
	pfree(lens);
	pfree(toasts);
	pfree(walbuf);
}

/* ------------------------------------------------------------------------
 * Concurrency checks for modifications and locks
 * ------------------------------------------------------------------------
 */
typedef struct IHCheck
{
	TM_Result	result;
	TransactionId xwait;		/* TM_BeingModified: whom to wait for */
	IHeapHdr	h;				/* item header (after rollback) */
	IHeapXidStatus status;		/* status of h.xid */
	bool		frozen;
	bool		newer;			/* version committed after caller's snapshot */
	bool		we_lock;		/* we hold a tuple lock */
	bool		we_locked_version;	/* ... taken on this very version (our
									 * LOCK record is on top of it) */
	bool		under_running_update;	/* KEY SHARE over a running non-key
										 * in-place update */
	bool		running_move;	/* KEY SHARE over a running non-key move
								 * (ctid: the new home) */
	UndoRecPtr	version_urec;	/* record that made h's version (below a
								 * LOCK record): identifies the version */
	LockTupleMode our_mode;
	int			nlockers;		/* other running, non-conflicting lockers */
	IHeapLocker lockers[IHEAP_MAX_LOCKERS];
	CommandId	cmax;
	ItemPointerData ctid;
	TransactionId modxid;
} IHCheck;

/*
 * Command id of the operation that created the item's current version, and
 * whether that operation inserted the row at this TID (insert, or the new
 * home of a moved row) rather than modifying a row already there.
 */
static CommandId
ih_creator_cid(Relation rel, const IHeapHdr *h, bool *inserted)
{
	IHeapUndoRec *rec = iheap_undo_fetch(rel, h->urec, false);
	CommandId	cid;

	if (rec->hdr.ur_type == IHU_LOCK)
	{
		IHeapHdr	ph;

		ih_hdr_read(rec->hdr.ur_prev_hdr, &ph);
		pfree(rec);
		rec = iheap_undo_fetch(rel, ph.urec, false);
	}
	cid = rec->hdr.ur_cid;
	if (inserted)
		*inserted = (rec->hdr.ur_type == IHU_INSERT ||
					 rec->hdr.ur_type == IHU_MULTI_INSERT);
	pfree(rec);
	return cid;
}

/* the TID a moved version went to */
static bool
ih_moved_to(Relation rel, const IHeapHdr *h, ItemPointer ctid)
{
	IHeapUndoRec *rec = iheap_undo_fetch_version_rec(rel, h);
	bool		found = false;

	if (rec != NULL && rec->hdr.ur_type == IHU_MOVE &&
		rec->payload_len >= sizeof(ItemPointerData))
	{
		memcpy(ctid, rec->payload, sizeof(ItemPointerData));
		found = true;
	}
	if (rec)
		pfree(rec);
	return found;
}

/*
 * Note whether the current transaction holds a lock on the item's current
 * version (c->h), in a LOCK record on top of it or carried into the record
 * of the change that made it: one who does must not queue for the row
 * (ih_wait).
 */
static void
ih_note_own_locks(Relation rel, IHCheck *c)
{
	IHeapUndoRec *rec;
	IHeapLocker lk[IHEAP_MAX_LOCKERS];
	int			n;

	if ((c->h.infomask & IHT_LOCKED) == 0 || c->frozen)
		return;
	rec = iheap_undo_fetch(rel, c->h.urec, true);
	if (rec == NULL)
		return;
	n = iheap_undo_lockers(rec, lk);
	pfree(rec);
	for (int i = 0; i < n; i++)
	{
		if (!TransactionIdIsCurrentTransactionId(lk[i].xid))
			continue;
		if (!c->we_lock || lk[i].mode > c->our_mode)
			c->our_mode = (LockTupleMode) lk[i].mode;
		c->we_lock = true;
	}
}

/*
 * Examine whether the current transaction may modify (or lock in "mode")
 * the item.  The buffer is exclusively locked.  Aborted versions are rolled
 * back first.
 *
 * nokey_mover: for a KEY SHARE locker following a row moved by a running
 * update that kept the key, the mover; its new version of the row does not
 * make the locker wait (see iheap_tuple_lock).
 */
static void
ih_check_modify_ext(Relation rel, Buffer buf, OffsetNumber off, CommandId cid,
					Snapshot snapshot, LockTupleMode mode,
					TransactionId nokey_mover, IHCheck *c)
{
	Page		page = BufferGetPage(buf);
	ItemId		lp;
	char	   *tup;

	memset(c, 0, sizeof(*c));
	c->result = TM_Ok;
	c->xwait = InvalidTransactionId;
	c->modxid = InvalidTransactionId;
	c->cmax = InvalidCommandId;
	ItemPointerSet(&c->ctid, BufferGetBlockNumber(buf), off);

	if (off < FirstOffsetNumber || off > PageGetMaxOffsetNumber(page))
	{
		c->result = TM_Deleted;
		return;
	}

	/*
	 * Find the current version.  If its creator aborted (possibly a moment
	 * ago, after the decision of an earlier rollback attempt), apply its
	 * undo and look again: an aborted version must never be treated as
	 * someone's committed or in-progress change.
	 */
	for (;;)
	{
		lp = PageGetItemId(page, off);
		if (!ItemIdIsNormal(lp))
		{
			c->result = TM_Deleted;
			return;
		}
		tup = (char *) page + ItemIdGetOffset(lp);
		ih_hdr_read(tup, &c->h);
		c->version_urec = c->h.urec;
		c->frozen = iheap_is_frozen(rel, &c->h);
		c->status = c->frozen ? IHX_COMMITTED : iheap_xid_status(c->h.xid, buf, tup);
		if (c->status != IHX_ABORTED)
			break;
		if (!iheap_rollback_tuple(rel, buf, off))
			elog(ERROR, "iheap: could not roll back aborted version of (%u,%u) in relation \"%s\"",
				 BufferGetBlockNumber(buf), off, RelationGetRelationName(rel));
		iheap_stat_rollback_report(RelationGetRelid(rel), IHEAP_ROLLBACK_LAZY);
	}

	if (c->h.infomask & (IHT_DELETED | IHT_MOVED))
	{
		c->modxid = c->h.xid;
		switch (c->status)
		{
			case IHX_CURRENT:
				{
					CommandId	dcid = ih_creator_cid(rel, &c->h, NULL);

					if (dcid >= cid)
					{
						c->result = TM_SelfModified;
						c->cmax = dcid;
					}
					else
						c->result = TM_Invisible;
					return;
				}
			case IHX_IN_PROGRESS:

				/*
				 * As heap, KEY SHARE does not wait for a running update that
				 * moved the row without changing its key either.  The caller
				 * locks this old version, which hands the lock to the
				 * restored version if the update rolls back, and then the new
				 * one, which keeps it if the update commits.
				 */
				if (mode == LockTupleKeyShare &&
					(c->h.infomask & (IHT_DELETED | IHT_KEYS_UPDATED | IHT_MOVED_PART)) == 0 &&
					ih_moved_to(rel, &c->h, &c->ctid))
				{
					c->running_move = true;
					break;
				}
				ih_note_own_locks(rel, c);
				c->result = TM_BeingModified;
				c->xwait = c->h.xid;
				return;
			default:
				break;
		}
		if (!c->running_move)
		{
			c->result = TM_Deleted;

			/* as heap: a row moved to another partition is "updated", nowhere */
			if (c->h.infomask & IHT_MOVED_PART)
			{
				c->result = TM_Updated;
				ItemPointerSetMovedPartitions(&c->ctid);
				return;
			}
			if ((c->h.infomask & IHT_MOVED) && !c->frozen &&
				ih_moved_to(rel, &c->h, &c->ctid))
				c->result = TM_Updated;
			return;
		}
	}

	/* a live version (or one being moved, for a KEY SHARE locker) */
	if (c->status == IHX_CURRENT)
	{
		bool		inserted;
		CommandId	ccid = ih_creator_cid(rel, &c->h, &inserted);

		if (ccid >= cid)
		{
			/*
			 * As heap: a row inserted by the current command is invisible
			 * to it; one it modified is self-modified.
			 */
			c->result = inserted ? TM_Invisible : TM_SelfModified;
			c->cmax = ccid;
			c->modxid = c->h.xid;
			return;
		}
	}
	else if (c->status == IHX_IN_PROGRESS && !c->running_move)
	{
		/*
		 * As heap, KEY SHARE does not conflict with a running non-key
		 * update: the lock goes on top of that update's version, and if
		 * the update aborts its rollback hands the lock to the restored
		 * version (iheap_relock_after_rollback).  The same holds for the
		 * new home of a row moved by such an update (nokey_mover).
		 */
		if (mode == LockTupleKeyShare &&
			(((c->h.infomask & IHT_INPLACE) != 0 &&
			  (c->h.infomask & IHT_KEYS_UPDATED) == 0) ||
			 (TransactionIdIsValid(nokey_mover) &&
			  TransactionIdEquals(c->h.xid, nokey_mover))))
			c->under_running_update = true;
		else
		{
			ih_note_own_locks(rel, c);
			c->result = TM_BeingModified;
			c->xwait = c->h.xid;
			c->modxid = c->h.xid;
			return;
		}
	}

	if ((c->h.infomask & IHT_LOCKED) && !c->frozen)
	{
		IHeapUndoRec *rec = iheap_undo_fetch(rel, c->h.urec, true);
		IHeapLocker lk[IHEAP_MAX_LOCKERS];
		int			n = rec ? iheap_undo_lockers(rec, lk) : 0;
		bool		lock_on_top = rec != NULL && rec->hdr.ur_type == IHU_LOCK;
		TransactionId conflict = InvalidTransactionId;

		if (lock_on_top)
		{
			IHeapHdr	ph;

			ih_hdr_read(rec->hdr.ur_prev_hdr, &ph);
			c->version_urec = ph.urec;
		}
		if (rec)
			pfree(rec);
		for (int i = 0; i < n; i++)
		{
			if (TransactionIdIsCurrentTransactionId(lk[i].xid))
			{
				if (!c->we_lock || lk[i].mode > c->our_mode)
					c->our_mode = (LockTupleMode) lk[i].mode;
				c->we_lock = true;

				/*
				 * A lock carried into a later version by someone else's
				 * update was taken on an older version.
				 */
				if (lock_on_top)
					c->we_locked_version = true;

				/*
				 * Our own entries stay in the list: one of an outer
				 * (sub)transaction must survive the rollback of an inner
				 * one that strengthens it, and a modification carries them
				 * (an UPDATE after a stronger lock of ours keeps that lock).
				 */
				if (c->nlockers < IHEAP_MAX_LOCKERS)
					c->lockers[c->nlockers++] = lk[i];
				continue;
			}
			if (!TransactionIdIsInProgress(lk[i].xid))
				continue;

			/*
			 * Look at the whole list before reporting a conflict: whether
			 * we hold a lock already decides how the caller waits.
			 */
			if (ih_modes_conflict((LockTupleMode) lk[i].mode, mode))
			{
				if (!TransactionIdIsValid(conflict))
					conflict = lk[i].xid;
				continue;
			}
			if (c->nlockers < IHEAP_MAX_LOCKERS)
				c->lockers[c->nlockers++] = lk[i];
		}
		if (TransactionIdIsValid(conflict))
		{
			c->result = TM_BeingModified;
			c->xwait = conflict;
			c->modxid = conflict;
			return;
		}
	}

	if (!c->frozen && c->status == IHX_COMMITTED && snapshot != NULL &&
		IsMVCCSnapshot(snapshot) && XidInMVCCSnapshot(c->h.xid, snapshot))
		c->newer = true;
}

static inline void
ih_check_modify(Relation rel, Buffer buf, OffsetNumber off, CommandId cid,
				Snapshot snapshot, LockTupleMode mode, IHCheck *c)
{
	ih_check_modify_ext(rel, buf, off, cid, snapshot, mode,
						InvalidTransactionId, c);
}

/*
 * Versions newer than the caller's snapshot that iheap_tuple_lock() handed
 * to a modifying caller: EvalPlanQual after an UPDATE or DELETE that found
 * the row changed, BEFORE ROW triggers, ON CONFLICT, MERGE, replication
 * apply.  Such a caller has rechecked the version it got and goes on to
 * modify it; the modification must then not report it as changed again, or
 * the caller retries forever.  In heap, the TID tells which version the
 * caller holds; an in-place update keeps the TID, so the lock remembers it.
 * SELECT FOR UPDATE/SHARE (TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS) grants
 * nothing: a later UPDATE in the same statement computed its new row from
 * the version its own scan saw, and must recheck.
 */
typedef struct IHLockGrant
{
	Oid			relid;
	ItemPointerData tid;
	UndoRecPtr	version;
	TransactionId xid;
	CommandId	cid;
} IHLockGrant;

#define IH_LOCK_GRANTS 8

static IHLockGrant ih_grants[IH_LOCK_GRANTS];
static int	ih_grant_next;

static void
ih_grant_version(Relation rel, ItemPointer tid, const IHCheck *c,
				 TransactionId xid, CommandId cid)
{
	IHLockGrant *g = NULL;

	for (int i = 0; i < IH_LOCK_GRANTS; i++)
	{
		if (ih_grants[i].relid == RelationGetRelid(rel) &&
			ItemPointerEquals(&ih_grants[i].tid, tid))
		{
			g = &ih_grants[i];
			break;
		}
	}
	if (g == NULL)
	{
		g = &ih_grants[ih_grant_next];
		ih_grant_next = (ih_grant_next + 1) % IH_LOCK_GRANTS;
	}
	g->relid = RelationGetRelid(rel);
	g->tid = *tid;
	g->version = c->version_urec;
	g->xid = xid;
	g->cid = cid;
}

/*
 * Was the version of tid identified by "version" handed to (sub)transaction
 * xid by a modifying lock, in command cid (InvalidCommandId: any)?
 */
bool
iheap_version_granted(Relation rel, ItemPointer tid, UndoRecPtr version,
					  TransactionId xid, CommandId cid)
{
	if (!TransactionIdIsValid(xid))
		return false;
	for (int i = 0; i < IH_LOCK_GRANTS; i++)
	{
		const IHLockGrant *g = &ih_grants[i];

		if (g->relid == RelationGetRelid(rel) && ItemPointerEquals(&g->tid, tid) &&
			g->version == version && TransactionIdEquals(g->xid, xid) &&
			(cid == InvalidCommandId || g->cid == cid))
			return true;
	}
	return false;
}

static bool
ih_version_granted(Relation rel, ItemPointer tid, const IHCheck *c,
				   TransactionId xid, CommandId cid)
{
	return c->we_lock &&
		iheap_version_granted(rel, tid, c->version_urec, xid, cid);
}

static void
ih_fill_tmfd(TM_FailureData *tmfd, IHCheck *c)
{
	tmfd->ctid = c->ctid;
	tmfd->xmax = c->modxid;
	tmfd->cmax = (c->result == TM_SelfModified) ? c->cmax : InvalidCommandId;
}

/*
 * Wait for xwait to finish (or for the lock to become available), queueing
 * on the heavyweight tuple lock first, as heap does.  Returns false if the
 * policy says to skip.
 *
 * As heap, a transaction that already holds a (weaker) lock on the row does
 * not queue: someone queued ahead of it may be waiting for it to finish.
 */
static bool
ih_wait(Relation rel, ItemPointer tid, TransactionId xwait, LockTupleMode mode,
		LockWaitPolicy policy, bool we_lock, LOCKMODE *held_tuple_lock,
		XLTW_Oper oper)
{
	/* the heavyweight tuple lock is taken once; the mode taken is remembered */
	if (*held_tuple_lock == NoLock && !we_lock)
	{
		switch (policy)
		{
			case LockWaitBlock:
				LockTuple(rel, tid, ih_hwlock[mode]);
				break;
			case LockWaitSkip:
				if (!ConditionalLockTuple(rel, tid, ih_hwlock[mode], false))
					return false;
				break;
			case LockWaitError:
				if (!ConditionalLockTuple(rel, tid, ih_hwlock[mode], log_lock_failures))
					ereport(ERROR,
							(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
							 errmsg("could not obtain lock on row in relation \"%s\"",
									RelationGetRelationName(rel))));
				break;
		}
		*held_tuple_lock = ih_hwlock[mode];
	}

	switch (policy)
	{
		case LockWaitBlock:
			XactLockTableWait(xwait, rel, tid, oper);
			break;
		case LockWaitSkip:
			if (!ConditionalXactLockTableWait(xwait, false))
				return false;
			break;
		case LockWaitError:
			if (!ConditionalXactLockTableWait(xwait, log_lock_failures))
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on row in relation \"%s\"",
								RelationGetRelationName(rel))));
			break;
	}
	return true;
}

/* crosscheck: is the current version visible to the crosscheck snapshot? */
static bool
ih_crosscheck_ok(Relation rel, Buffer buf, OffsetNumber off, Snapshot crosscheck)
{
	IHeapVersion v;
	bool		ok;

	if (crosscheck == InvalidSnapshot)
		return true;
	ok = iheap_fetch_version(rel, buf, BufferGetPage(buf), off, crosscheck, &v);
	if (ok && v.from_undo)
	{
		/* the crosscheck snapshot sees an older version */
		if (v.mem)
			pfree(v.mem);
		ok = false;
	}
	return ok;
}

/*
 * Put a LOCK record listing exactly "lockers" on top of the item's current
 * version and point the item at it; the buffer is exclusively locked.  The
 * record's prev header is the version's own header (an existing LOCK record
 * on top is replaced, keeping the chain one hop long, so callers include
 * its lockers that still matter).  xid is the record's writer: the current
 * transaction (the record joins its chain) or, for a lock re-attached by
 * rollback on behalf of others, one of the lockers.  Returns the new header.
 */
IHeapHdr
iheap_put_lock_record(Relation rel, Buffer buf, OffsetNumber off,
					  const IHeapLocker *lockers, int nlockers,
					  TransactionId xid, CommandId cid)
{
	Page		page = BufferGetPage(buf);
	ItemId		lp = PageGetItemId(page, off);
	char	   *tup = (char *) page + ItemIdGetOffset(lp);
	char		payload[IHEAP_MAX_LOCKERS * IHEAP_LOCKER_SIZE + sizeof(uint16)];
	uint32		payload_len;
	IHeapUndoRecHeader uh;
	IHeapUndoReservation res;
	UndoRecPtr	urp;
	IHeapHdr	cur;
	IHeapHdr	nh;
	char		prevhdr[IHEAP_TUPHDR_SIZE];
	bool		ours = TransactionIdIsCurrentTransactionId(xid);

	Assert(nlockers > 0 && nlockers <= IHEAP_MAX_LOCKERS);
	payload_len = iheap_lockers_size(nlockers);
	iheap_lockers_serialize(payload, lockers, nlockers);

	ih_hdr_read(tup, &cur);
	ih_hdr_write_image(prevhdr, &cur);
	if (cur.infomask & IHT_LOCKED)
	{
		IHeapUndoRec *rec = iheap_undo_fetch(rel, cur.urec, true);

		if (rec != NULL && rec->hdr.ur_type == IHU_LOCK)
		{
			IHeapHdr	ph;

			/* keep current hint bits; they describe the same creator */
			ih_hdr_read(rec->hdr.ur_prev_hdr, &ph);
			ph.infomask |= (cur.infomask & IHT_XID_HINTS);
			ih_hdr_write_image(prevhdr, &ph);
		}
		if (rec)
			pfree(rec);
	}

	memset(&uh, 0, sizeof(uh));
	uh.ur_type = IHU_LOCK;
	uh.ur_flags = IHUF_LOCKERS;
	uh.ur_xid = xid;
	uh.ur_cid = cid;
	uh.ur_blkno = BufferGetBlockNumber(buf);
	uh.ur_offnum = off;
	memcpy(uh.ur_prev_hdr, prevhdr, IHEAP_TUPHDR_SIZE);

	memset(&res, 0, sizeof(res));
	iheap_undo_reserve(rel, IHEAP_UNDOREC_HDRSZ + payload_len, &res);

	START_CRIT_SECTION();
	uh.ur_xact_prev = ours ? iheap_xact_last_urp(rel) : InvalidUndoRecPtr;
	urp = iheap_undo_write(rel, &res, &uh, payload, payload_len);

	nh = cur;
	nh.urec = urp;
	nh.infomask |= IHT_LOCKED;
	ih_hdr_write(tup, &nh);
	MarkBufferDirty(buf);

	if (RelationNeedsWAL(rel))
	{
		xl_iheap_hdrchange xlrec;
		XLogRecPtr	recptr;
		char		change[SizeOfIHeapWalHdrChange];

		xlrec.offnum = off;
		xlrec.flags = 0;
		iheap_walchange_put(change, &nh);
		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfIHeapHdrChange);
		XLogRegisterBuffer(IH_BLK_DATA, buf, REGBUF_STANDARD);
		XLogRegisterBufData(IH_BLK_DATA, change, SizeOfIHeapWalHdrChange);
		iheap_undo_register_wal(&res, IH_BLK_UNDO,
								BufferGetBlockNumber(buf), false);
		recptr = XLogInsert(RM_IHEAP_ID, XLOG_IHEAP_LOCK);
		PageSetLSN(page, recptr);
		iheap_undo_set_lsn(&res, recptr);
	}
	END_CRIT_SECTION();

	if (ours)
		iheap_xact_register_write(rel, urp, uh.ur_len);
	iheap_undo_release(&res);
	return nh;
}

/*
 * Write a LOCK record making "mode" held by the current transaction, and
 * point the item at it.  c describes the item (from ih_check_modify).
 */
static void
ih_add_lock(Relation rel, Buffer buf, OffsetNumber off, IHCheck *c,
			LockTupleMode mode, TransactionId xid, CommandId cid)
{
	IHeapLocker lk[IHEAP_MAX_LOCKERS];
	int			n = 0;
	LockTupleMode newmode = mode;

	/* other lockers (ours of outer (sub)transactions too); ours replaced */
	for (int i = 0; i < c->nlockers && n < IHEAP_MAX_LOCKERS - 1; i++)
	{
		if (TransactionIdEquals(c->lockers[i].xid, xid))
		{
			newmode = Max(newmode, (LockTupleMode) c->lockers[i].mode);
			continue;
		}
		lk[n++] = c->lockers[i];
	}
	lk[n].xid = xid;
	lk[n].mode = (uint8) newmode;
	n++;

	c->h = iheap_put_lock_record(rel, buf, off, lk, n, xid, cid);
	c->we_lock = true;
	c->we_locked_version = true;
	c->our_mode = Max(c->our_mode, newmode);
}

/*
 * Rollback replaced an aborted version under LOCK records whose lockers are
 * still running (a KEY SHARE lock may be taken on top of a running non-key
 * update): give those locks to the restored version, merged with its own.
 */
void
iheap_relock_after_rollback(Relation rel, Buffer buf, OffsetNumber off,
							const IHeapLocker *keep, int nkeep)
{
	Page		page = BufferGetPage(buf);
	ItemId		lp;
	IHeapHdr	h;
	IHeapLocker all[IHEAP_MAX_LOCKERS];
	int			n = 0;

	if (off < FirstOffsetNumber || off > PageGetMaxOffsetNumber(page))
		return;
	lp = PageGetItemId(page, off);
	if (!ItemIdIsNormal(lp))
		return;
	ih_hdr_read((char *) page + ItemIdGetOffset(lp), &h);
	if (h.infomask & (IHT_DELETED | IHT_MOVED | IHT_STUB))
		return;					/* a lock on a row that is gone protects nothing */

	/* the restored version's own running lockers */
	if ((h.infomask & IHT_LOCKED) && !iheap_is_frozen(rel, &h))
	{
		IHeapUndoRec *rec = iheap_undo_fetch(rel, h.urec, true);

		if (rec != NULL)
		{
			IHeapLocker lk[IHEAP_MAX_LOCKERS];
			int			m = iheap_undo_lockers(rec, lk);

			for (int i = 0; i < m; i++)
				if (iheap_xid_running(lk[i].xid))
					all[n++] = lk[i];
			pfree(rec);
		}
	}

	for (int i = 0; i < nkeep; i++)
	{
		int			j;

		if (!iheap_xid_running(keep[i].xid))
			continue;
		for (j = 0; j < n; j++)
		{
			if (TransactionIdEquals(all[j].xid, keep[i].xid))
			{
				all[j].mode = Max(all[j].mode, keep[i].mode);
				break;
			}
		}
		if (j == n && n < IHEAP_MAX_LOCKERS)
			all[n++] = keep[i];
	}

	if (n > 0)
		(void) iheap_put_lock_record(rel, buf, off, all, n, all[0].xid,
									 FirstCommandId);
}

/* ------------------------------------------------------------------------
 * DELETE
 * ------------------------------------------------------------------------
 */
TM_Result
iheap_tuple_delete(Relation rel, ItemPointer tid, CommandId cid,
				   uint32 options, Snapshot snapshot, Snapshot crosscheck,
				   bool wait, TM_FailureData *tmfd)
{
	BlockNumber blk = ItemPointerGetBlockNumber(tid);
	OffsetNumber off = ItemPointerGetOffsetNumber(tid);
	Buffer		buf;
	Page		page;
	IHCheck		c;
	IHVmState	vm = IHVM_INIT;
	LOCKMODE	held_tuple_lock = NoLock;
	TransactionId xid;
	ItemId		lp;
	char	   *tup;
	uint32		len;
	IHeapUndoRecHeader uh;
	IHeapUndoReservation res;
	UndoRecPtr	urp;
	IHeapHdr	nh;
	char	   *oldcopy = NULL;
	char	   *oldmem = NULL;
	bool		logical = iheap_logically_logged(rel,
												 (options & TABLE_DELETE_NO_LOGICAL) != 0);
	IHeapLogicalPart logpart = {0};
	bool		compact;
	uint32		hs;

	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot delete tuples during a parallel operation")));

	ih_maybe_discard(rel);
	xid = GetCurrentTransactionId();

	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
	ih_lock_with_vm(rel, buf, &vm);
	page = BufferGetPage(buf);
	if (!IHeapPageIsData(page))
		elog(ERROR, "iheap: tid (%u,%u) does not point to a data page", blk, off);

	for (;;)
	{
		ih_check_modify(rel, buf, off, cid, snapshot, LockTupleExclusive, &c);
		if (c.result == TM_Ok && c.newer &&
			!ih_version_granted(rel, tid, &c, xid, cid))
		{
			c.result = TM_Updated;
			c.modxid = c.h.xid;
		}
		if (c.result != TM_BeingModified)
			break;
		if (!wait)
			break;
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		(void) ih_wait(rel, tid, c.xwait, LockTupleExclusive, LockWaitBlock,
					   c.we_lock, &held_tuple_lock, XLTW_Delete);
		ih_lock_with_vm(rel, buf, &vm);
	}

	if (c.result == TM_Ok && !ih_crosscheck_ok(rel, buf, off, crosscheck))
	{
		c.result = TM_Updated;
		c.modxid = c.h.xid;
	}

	if (c.result != TM_Ok)
	{
		ih_fill_tmfd(tmfd, &c);
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buf);
		ih_vm_finish(&vm);
		if (held_tuple_lock != NoLock)
			UnlockTuple(rel, tid, held_tuple_lock);
		return c.result;
	}

	lp = PageGetItemId(page, off);
	tup = (char *) page + ItemIdGetOffset(lp);
	len = ItemIdGetLength(lp);
	if (c.h.infomask & IHT_HASEXTERNAL)
		oldcopy = ih_copy_tuple_aligned(CurrentMemoryContext, tup, len, &oldmem);

	/*
	 * The old row's replica identity for logical decoding, as heap logs it
	 * (fetching values stored out of line under the buffer lock, as heap).
	 */
	if (logical)
	{
		TupleDesc	desc = RelationGetDescr(rel);
		Datum	   *values = palloc(Max(desc->natts, 1) * sizeof(Datum));
		bool	   *isnull = palloc(Max(desc->natts, 1) * sizeof(bool));

		ih_deform_tuple(desc, tup, values, isnull);
		iheap_logical_prepare(&logpart, rel, false, values, isnull, true);
		pfree(values);
		pfree(isnull);
	}

	CheckForSerializableConflictIn(rel, tid, blk);

	/*
	 * The deleted row's body stays on the page until no snapshot can see it
	 * any more, as heap's does: the undo record needs only the header.  A
	 * compact header has no room for the deleter's xid: then the body goes to
	 * undo and the item becomes a stub, its storage kept until the deletion
	 * commits.
	 */
	compact = (c.h.infomask & IHT_COMPACT) != 0;
	hs = ih_hdr_size(tup);
	memset(&uh, 0, sizeof(uh));
	uh.ur_type = IHU_DELETE;
	uh.ur_xid = xid;
	uh.ur_cid = cid;
	uh.ur_blkno = blk;
	uh.ur_offnum = off;
	ih_hdr_image(uh.ur_prev_hdr, tup);

	memset(&res, 0, sizeof(res));
	iheap_undo_reserve(rel, IHEAP_UNDOREC_HDRSZ + (compact ? len - hs : 0), &res);
	ih_vm_prepare(rel, buf, &vm);

	/* the page may have been compacted while we waited for undo space */
	lp = PageGetItemId(page, off);
	tup = (char *) page + ItemIdGetOffset(lp);

	START_CRIT_SECTION();
	uh.ur_xact_prev = iheap_xact_last_urp(rel);
	urp = iheap_undo_write(rel, &res, &uh, compact ? tup + hs : NULL,
						   compact ? len - hs : 0);

	nh = c.h;
	nh.xid = xid;
	nh.urec = urp;
	nh.infomask = (c.h.infomask & IHT_DATA_FLAGS) | IHT_DELETED | IHT_KEYS_UPDATED |
		((options & TABLE_DELETE_CHANGING_PARTITION) ? IHT_MOVED_PART : 0);
	if (compact)
	{
		nh.infomask |= IHT_STUB;
		nh.hoff += IHEAP_TUPHDR_SIZE - IHEAP_COMPACT_HDRSZ;
		ih_hdr_write(tup, &nh);
		ItemIdSetNormal(lp, ItemIdGetOffset(lp), IHEAP_TUPHDR_SIZE);
		iheap_page_note_room(page, xid);
	}
	else
		ih_hdr_write(tup, &nh);
	IHeapPageGetOpaque(page)->ihp_flags |= IHP_PRUNABLE;
	ih_vm_clear(rel, buf, &vm);
	MarkBufferDirty(buf);

	if (RelationNeedsWAL(rel))
	{
		xl_iheap_hdrchange xlrec;
		XLogRecPtr	recptr;
		char		change[SizeOfIHeapWalHdrChange];

		xlrec.offnum = off;
		xlrec.flags = (vm.cleared ? XLIH_VM_CLEARED : 0) |
			(logical ? XLIH_LOGICAL : 0);
		iheap_walchange_put(change, &nh);
		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfIHeapHdrChange);
		if (logical)
			XLogRegisterData(logpart.data, logpart.len);
		XLogRegisterBuffer(IH_BLK_DATA, buf, REGBUF_STANDARD);
		XLogRegisterBufData(IH_BLK_DATA, change, SizeOfIHeapWalHdrChange);
		iheap_undo_register_wal(&res, IH_BLK_UNDO,
								BufferGetBlockNumber(buf), false);
		if (vm.cleared)
			XLogRegisterBuffer(IH_BLK_VM, vm.vmbuf, 0);
		recptr = XLogInsert(RM_IHEAP_ID, XLOG_IHEAP_DELETE);
		PageSetLSN(page, recptr);
		iheap_undo_set_lsn(&res, recptr);
		if (vm.cleared)
			PageSetLSN(BufferGetPage(vm.vmbuf), recptr);
	}
	END_CRIT_SECTION();

	iheap_xact_register_write(rel, urp, uh.ur_len);
	iheap_undo_release(&res);
	ih_vm_finish(&vm);
	UnlockReleaseBuffer(buf);
	if (held_tuple_lock != NoLock)
		UnlockTuple(rel, tid, held_tuple_lock);
	iheap_logical_free(&logpart);

	if (oldcopy)
	{
		iheap_toast_delete_values(rel, RelationGetDescr(rel), oldcopy, false);
		pfree(oldmem);
	}

	pgstat_count_heap_delete(rel);
	return TM_Ok;
}

/* ------------------------------------------------------------------------
 * UPDATE
 * ------------------------------------------------------------------------
 */

/* attributes whose binary value differs between the two versions */
static Bitmapset *
ih_modified_attrs(TupleDesc desc, const Datum *oldv, const bool *oldn,
				  const Datum *newv, const bool *newn)
{
	Bitmapset  *mod = NULL;

	for (int i = 0; i < desc->natts; i++)
	{
		CompactAttribute *att = TupleDescCompactAttr(desc, i);
		bool		differ;

		if (att->attisdropped)
			continue;
		if (oldn[i] != newn[i])
			differ = true;
		else if (oldn[i])
			differ = false;
		else
			differ = !datum_image_eq(oldv[i], newv[i], att->attbyval, att->attlen);
		if (differ)
			mod = bms_add_member(mod, i + 1 - FirstLowInvalidHeapAttributeNumber);
	}
	/* a whole-row reference (an index on f(t.*)) changes with any column */
	if (mod != NULL)
		mod = bms_add_member(mod, 0 - FirstLowInvalidHeapAttributeNumber);
	return mod;
}

/*
 * Does an ancestor (partitioned table or inheritance parent) have an UPDATE
 * trigger with an OLD transition table?  Statement triggers are not cloned
 * to partitions or children, but an UPDATE through the ancestor captures
 * OLD rows from them by refetching TIDs, just as for the relation's own
 * triggers.  The answer is cached per top-level statement (an ancestor's
 * triggers cannot change during an UPDATE through it, and do not matter for
 * an UPDATE on the child itself).
 */
typedef struct AncestorCacheEnt
{
	Oid			relid;
	TimestampTz stmt;			/* top-level statement ... */
	TimestampTz xact;			/* ... and transaction it was computed in */
	bool		capture;
} AncestorCacheEnt;

static HTAB *ancestor_cache = NULL;

static void
ih_parents_of(Relation rel, Oid relid, List **out)
{
	Relation	inh;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tup;

	inh = table_open(InheritsRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_inherits_inhrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(inh, InheritsRelidSeqnoIndexId, true, NULL, 1, &key);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Oid			parent = ((Form_pg_inherits) GETSTRUCT(tup))->inhparent;

		if (!list_member_oid(*out, parent))
		{
			*out = lappend_oid(*out, parent);
			ih_parents_of(rel, parent, out);
		}
	}
	systable_endscan(scan);
	table_close(inh, AccessShareLock);
}

static bool
ih_ancestors_capture_old(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	TimestampTz stmt = GetCurrentStatementStartTimestamp();
	TimestampTz xact = GetCurrentTransactionStartTimestamp();
	AncestorCacheEnt *ent;
	bool		found;
	List	   *ancestors = NIL;
	ListCell   *lc;

	if (ancestor_cache == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(AncestorCacheEnt);
		ctl.hcxt = TopMemoryContext;
		ancestor_cache = hash_create("iheap ancestor triggers", 32, &ctl,
									 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	ent = hash_search(ancestor_cache, &relid, HASH_ENTER, &found);
	if (found && ent->stmt == stmt && ent->xact == xact)
		return ent->capture;

	ent->stmt = stmt;
	ent->xact = xact;
	ent->capture = false;
	if (!rel->rd_rel->relispartition && !has_superclass(relid))
		return false;
	ih_parents_of(rel, relid, &ancestors);
	foreach(lc, ancestors)
	{
		Relation	a = try_relation_open(lfirst_oid(lc), AccessShareLock);

		if (a == NULL)
			continue;
		if (a->trigdesc && a->trigdesc->trig_update_old_table)
			ent->capture = true;
		relation_close(a, AccessShareLock);
		if (ent->capture)
			break;
	}
	list_free(ancestors);
	return ent->capture;
}

/*
 * Rule 3: AFTER ROW UPDATE triggers refetch OLD and NEW by TID (trigger.c),
 * which cannot work when both share a TID.
 */
static bool
ih_triggers_block_inplace(Relation rel, TupleDesc desc, const char *oldtup,
						  uint32 oldlen, TupleTableSlot *newslot)
{
	TriggerDesc *td = rel->trigdesc;
	TupleTableSlot *oldslot = NULL;
	bool		block = false;

	/* partitions and inheritance children: their ancestors' triggers */
	if (ih_ancestors_capture_old(rel))
		return true;

	if (td == NULL)
		return false;

	/*
	 * Transition tables capture OLD rows by refetching the TID after the
	 * update, which an in-place update has overwritten.
	 */
	if (td->trig_update_old_table)
		return true;

	if (!td->trig_update_after_row)
		return false;

	for (int i = 0; i < td->numtriggers && !block; i++)
	{
		Trigger    *t = &td->triggers[i];

		if (!TRIGGER_TYPE_MATCHES(t->tgtype, TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_AFTER, TRIGGER_TYPE_UPDATE))
			continue;
		if (t->tgenabled == TRIGGER_DISABLED)
			continue;
		if (t->tgfoid == F_UNIQUE_KEY_RECHECK)
			continue;			/* fires only after index insertions */

		switch (RI_FKey_trigger_type(t->tgfoid))
		{
			case RI_TRIGGER_FK:
				/* RI_FKey_check reads NEW only */
				break;
			case RI_TRIGGER_PK:
				if (oldslot == NULL)
				{
					oldslot = MakeSingleTupleTableSlot(desc, &TTSOpsIHeapTuple);
					iheap_store_tuple(oldslot, (char *) oldtup, oldlen, true,
									  InvalidTransactionId, InvalidTransactionId,
									  InvalidCommandId);
				}
				if (RI_FKey_pk_upd_check_required(t, rel, oldslot, newslot))
					block = true;
				break;
			default:
				block = true;
				break;
		}
	}

	if (oldslot)
		ExecDropSingleTupleTableSlot(oldslot);
	return block;
}

/* common prefix/suffix of two tuple bodies */
static void
ih_delta(const char *oldb, uint32 oldlen, const char *newb, uint32 newlen,
		 uint16 *prefix, uint16 *suffix)
{
	uint32		minlen = Min(oldlen, newlen);
	uint32		p = 0;
	uint32		s = 0;

	while (p < minlen && oldb[p] == newb[p])
		p++;
	while (s < minlen - p && oldb[oldlen - 1 - s] == newb[newlen - 1 - s])
		s++;
	*prefix = (uint16) p;
	*suffix = (uint16) s;
}

/* precomputed facts about an update, done without holding the page lock */
typedef struct IHUpdatePlan
{
	IHeapHdr	seen;			/* header of the version the plan is for */
	uint32		seenlen;
	Datum	   *oldvalues;
	bool	   *oldisnull;
	char	   *oldtup;
	char	   *oldmem;
	uint32		oldlen;
	Bitmapset  *modified;
	bool		key_changed;
	bool		inplace;
	IHeapMoveReason move_reason;	/* why not in place, if not */
	bool		summarized;
	bool		dm_changed;		/* a column a dmbtree index reads changes */
	bool		valid;
} IHUpdatePlan;

static void
ih_plan_free(IHUpdatePlan *p)
{
	if (p->oldmem)
		pfree(p->oldmem);
	if (p->oldvalues)
		pfree(p->oldvalues);
	if (p->oldisnull)
		pfree(p->oldisnull);
	bms_free(p->modified);
	memset(p, 0, sizeof(*p));
}

static void
ih_plan_update(Relation rel, Buffer buf, OffsetNumber off,
			   const Datum *newv, const bool *newn, TupleTableSlot *newslot,
			   IHUpdatePlan *p)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Page		page = BufferGetPage(buf);
	ItemId		lp;
	IHeapDmInfo *dm;
	Bitmapset  *sum_attrs;
	Bitmapset  *key_attrs;

	ih_plan_free(p);

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	if (off > PageGetMaxOffsetNumber(page) ||
		!ItemIdIsNormal(lp = PageGetItemId(page, off)))
	{
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		return;
	}
	ih_hdr_read((char *) page + ItemIdGetOffset(lp), &p->seen);
	p->seenlen = ItemIdGetLength(lp);
	p->oldlen = ItemIdGetLength(lp);
	p->oldtup = ih_copy_tuple_aligned(CurrentMemoryContext,
									  (char *) page + ItemIdGetOffset(lp),
									  p->oldlen, &p->oldmem);
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	if (p->seen.infomask & (IHT_DELETED | IHT_MOVED))
		return;					/* the check under lock will report it */

	p->oldvalues = palloc(Max(desc->natts, 1) * sizeof(Datum));
	p->oldisnull = palloc(Max(desc->natts, 1) * sizeof(bool));
	ih_deform_tuple(desc, p->oldtup, p->oldvalues, p->oldisnull);

	/*
	 * A change of a column some index covers moves the row, unless the
	 * column is a key column of dmbtree indexes only, which take in-place
	 * updates of their keys.
	 */
	dm = iheap_dm_info(rel);
	sum_attrs = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_SUMMARIZED);
	key_attrs = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_KEY);

	p->modified = ih_modified_attrs(desc, p->oldvalues, p->oldisnull, newv, newn);
	p->key_changed = bms_overlap(p->modified, key_attrs);
	p->summarized = bms_overlap(p->modified, sum_attrs);
	if (!iheap_enable_inplace)
		p->move_reason = IHEAP_MOVE_SETTING;
	else if (bms_overlap(p->modified, dm->blocking))
		p->move_reason = IHEAP_MOVE_INDEX;
	else if (ih_triggers_block_inplace(rel, desc, p->oldtup, p->oldlen, newslot))
		p->move_reason = IHEAP_MOVE_TRIGGER;
	else
		p->move_reason = IHEAP_MOVE_NONE;
	p->inplace = (p->move_reason == IHEAP_MOVE_NONE);
	p->dm_changed = p->inplace && bms_overlap(p->modified, dm->dmattrs);
	p->valid = true;

	bms_free(sum_attrs);
	bms_free(key_attrs);
}

TM_Result
iheap_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot,
				   CommandId cid, uint32 options, Snapshot snapshot,
				   Snapshot crosscheck, bool wait, TM_FailureData *tmfd,
				   LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	TupleDesc	desc = RelationGetDescr(rel);
	BlockNumber blk = ItemPointerGetBlockNumber(otid);
	OffsetNumber off = ItemPointerGetOffsetNumber(otid);
	Datum	   *newvalues;
	bool	   *newisnull;
	Buffer		buf;
	Buffer		newbuf = InvalidBuffer;
	Page		page;
	IHCheck		c;
	IHUpdatePlan plan;
	IHVmState	vm = IHVM_INIT;
	IHVmState	newvm = IHVM_INIT;
	LOCKMODE	held_tuple_lock = NoLock;
	TransactionId xid;
	LockTupleMode mode;
	IHeapToastState *ts = NULL;
	char	   *newtup;
	char	   *newmem;
	uint32		newlen;
	bool		inplace;
	ItemId		lp;
	char	   *tup;
	uint32		curlen;
	bool		logical = iheap_logically_logged(rel,
												 (options & TABLE_UPDATE_NO_LOGICAL) != 0);
	IHeapLogicalPart logpart = {0};
	bool		dm_entries = false;
	bool		grown = false;	/* the item's storage takes the new version */
	bool		keep_align = false; /* relocated, keeping the old alignment */

	*update_indexes = TU_None;
	*lockmode = LockTupleExclusive;

	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot update tuples during a parallel operation")));

	ih_maybe_discard(rel);
	xid = GetCurrentTransactionId();

	ih_copy_slot_values(slot, desc->natts, &newvalues, &newisnull);
	memset(&plan, 0, sizeof(plan));

	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
	page = BufferGetPage(buf);

	for (;;)
	{
		ih_plan_update(rel, buf, off, newvalues, newisnull, slot, &plan);
		mode = plan.key_changed ? LockTupleExclusive : LockTupleNoKeyExclusive;

		/*
		 * Set even when the update fails: on TM_Updated the executor locks
		 * the newest version in this mode before retrying (EvalPlanQual), and
		 * a weaker lock would let another waiter update the row meanwhile.
		 */
		*lockmode = mode;

		ih_lock_with_vm(rel, buf, &vm);
		if (!IHeapPageIsData(page))
			elog(ERROR, "iheap: tid (%u,%u) does not point to a data page", blk, off);

		ih_check_modify(rel, buf, off, cid, snapshot, mode, &c);
		if (c.result == TM_Ok && c.newer &&
			!ih_version_granted(rel, otid, &c, xid, cid))
		{
			c.result = TM_Updated;
			c.modxid = c.h.xid;
		}

		if (c.result == TM_BeingModified && wait)
		{
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			(void) ih_wait(rel, otid, c.xwait, mode, LockWaitBlock, c.we_lock,
						   &held_tuple_lock, XLTW_Update);
			continue;
		}

		/* the plan must describe the version we are about to replace */
		if (c.result == TM_Ok &&
			(!plan.valid || plan.seen.xid != c.h.xid ||
			 plan.seen.urec != c.h.urec ||
			 plan.seenlen != ItemIdGetLength(PageGetItemId(page, off))))
		{
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			continue;
		}
		break;
	}

	if (c.result == TM_Ok && !ih_crosscheck_ok(rel, buf, off, crosscheck))
	{
		c.result = TM_Updated;
		c.modxid = c.h.xid;
	}

	if (c.result != TM_Ok)
	{
		ih_fill_tmfd(tmfd, &c);
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buf);
		ih_vm_finish(&vm);
		if (held_tuple_lock != NoLock)
			UnlockTuple(rel, otid, held_tuple_lock);
		ih_plan_free(&plan);
		pfree(newvalues);
		pfree(newisnull);
		return c.result;
	}

	*lockmode = mode;
	inplace = plan.inplace;

	CheckForSerializableConflictIn(rel, otid, blk);

	/*
	 * TOAST work can do a lot of I/O: lock the row for us, release the page,
	 * toast, and lock the page again (others wait for our row lock).
	 */
	if (iheap_tuple_needs_toast(rel, desc, newvalues, newisnull,
								(c.h.infomask & IHT_HASEXTERNAL) != 0))
	{
		if (!c.we_lock || c.our_mode < mode)
			ih_add_lock(rel, buf, off, &c, mode, xid, cid);
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		ts = palloc(sizeof(IHeapToastState));
		iheap_toast_begin(ts, rel, desc, newvalues, newisnull,
						  plan.oldvalues, plan.oldisnull, 0);
		ih_lock_with_vm(rel, buf, &vm);
		ih_check_modify(rel, buf, off, cid, NULL, mode, &c);
		if (c.result != TM_Ok)
			elog(ERROR, "iheap: row changed while locked for update");
	}

	newtup = ih_form_tuple(desc, newvalues, newisnull, &newlen, &newmem);
	if (newlen > IHEAP_MAX_TUPLE_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("row is too big: size %u, maximum size %zu",
						newlen, (Size) IHEAP_MAX_TUPLE_SIZE)));

	/*
	 * For logical decoding, the old row's replica identity, as heap logs it
	 * (plan describes the version being replaced); the new tuple goes into
	 * the record whole, with the layout to read it by.
	 */
	if (logical)
		iheap_logical_prepare(&logpart, rel, true, plan.oldvalues,
							  plan.oldisnull,
							  iheap_logical_key_required(rel, plan.modified,
														 plan.oldvalues,
														 plan.oldisnull));

	lp = PageGetItemId(page, off);
	curlen = ItemIdGetLength(lp);

	/*
	 * The new version is written over the old one, unless it is larger or
	 * its data area would not be aligned there (gaining or losing its null
	 * bitmap changes its header length): then it goes to the page's free
	 * space, with room for the old version, which a rollback puts back in
	 * its place.  That place must suit every version a rollback may put
	 * back: the old one, and older ones whose changes have not committed.
	 * Their alignment suits the old version's place, so the new place keeps
	 * the old one's offset modulo MAXIMUM_ALIGNOF when one of them needs
	 * alignment; where the new version cannot be aligned at such a place, the
	 * row moves.
	 */
	if (inplace &&
		(newlen > curlen ||
		 !iheap_tuple_aligned_at(newtup, ItemIdGetOffset(lp))))
	{
		bool		older_align;
		uint32		cap = Max(Max(newlen, curlen),
							  iheap_item_reservation(rel, page, off, &older_align));
		PageHeader	ph = (PageHeader) page;

		keep_align = older_align ||
			ih_tuple_needs_align((char *) page + ItemIdGetOffset(lp));
		if (keep_align && !iheap_tuple_aligned_at(newtup, ItemIdGetOffset(lp)))
			inplace = false;
		else if ((int) ph->pd_upper - (int) ph->pd_lower < (int) (cap + MAXIMUM_ALIGNOF))
		{
			/*
			 * Too little room to put the new version elsewhere on the page.
			 * A version without aligned values can still grow where it is:
			 * compaction moves the tuples after it (a row whose compact
			 * header becomes a full one grows by 4 bytes).
			 */
			if (newlen > curlen && !ih_tuple_needs_align(newtup))
				grown = iheap_page_prune_grow(rel, buf, off, newlen);
			else
				(void) iheap_page_prune(rel, buf, true, NULL, NULL);
			lp = PageGetItemId(page, off);
			curlen = ItemIdGetLength(lp);
			cap = Max(Max(newlen, curlen),
					  iheap_item_reservation(rel, page, off, NULL));
			if (!grown &&
				((int) ph->pd_upper - (int) ph->pd_lower < (int) (cap + MAXIMUM_ALIGNOF) ||
				 (keep_align && !iheap_tuple_aligned_at(newtup, ItemIdGetOffset(lp)))))
				inplace = false;
		}
	}

	if (inplace)
	{
		/* ---- in-place update ---- */
		IHeapUndoRecHeader uh;
		IHeapUndoReservation res;
		IHeapUpdateDelta d;
		char	   *payload;
		uint32		payload_len;
		uint32		oldbody;
		uint32		newbody;
		uint32		curhs;
		uint32		newhs = ih_hdr_size(newtup);
		UndoRecPtr	urp;
		IHeapHdr	nh;
		uint16		lp_off;
		uint32		capacity = 0;
		bool		relocated;
		IHeapLocker carried[IHEAP_MAX_LOCKERS];
		int			ncarried = c.nlockers;

		/* the delta of the bodies: a compact header grows */
		tup = (char *) page + ItemIdGetOffset(lp);
		curhs = ih_hdr_size(tup);
		oldbody = curlen - curhs;
		newbody = newlen - newhs;
		ih_delta(tup + curhs, oldbody, newtup + newhs, newbody,
				 &d.prefix, &d.suffix);
		d.old_len = (uint16) curlen;
		d.mid_len = (uint16) (oldbody - d.prefix - d.suffix);
		memcpy(carried, c.lockers, ncarried * sizeof(IHeapLocker));

		payload_len = sizeof(d) + d.mid_len +
			(ncarried > 0 ? iheap_lockers_size(ncarried) : 0);
		payload = palloc(payload_len);
		memcpy(payload, &d, sizeof(d));
		memcpy(payload + sizeof(d), tup + curhs + d.prefix, d.mid_len);
		if (ncarried > 0)
			iheap_lockers_serialize(payload + sizeof(d) + d.mid_len, carried,
									ncarried);

		memset(&uh, 0, sizeof(uh));
		uh.ur_type = IHU_UPDATE;
		uh.ur_flags = (plan.key_changed ? IHUF_KEYS_UPDATED : 0) |
			(ncarried > 0 ? IHUF_LOCKERS : 0);
		uh.ur_xid = xid;
		uh.ur_cid = cid;
		uh.ur_blkno = blk;
		uh.ur_offnum = off;
		ih_hdr_image(uh.ur_prev_hdr, tup);

		/* before reserving undo: this reads undo, maybe our own page */
		relocated = !grown &&
			(newlen > curlen ||
			 !iheap_tuple_aligned_at(newtup, ItemIdGetOffset(lp)));
		if (relocated)
			capacity = Max(Max(newlen, curlen),
						   iheap_item_reservation(rel, page, off, NULL));

		memset(&res, 0, sizeof(res));
		iheap_undo_reserve(rel, IHEAP_UNDOREC_HDRSZ + payload_len, &res);
		ih_vm_prepare(rel, buf, &vm);

		/* reserve() may not move tuples, but re-derive pointers anyway */
		lp = PageGetItemId(page, off);
		tup = (char *) page + ItemIdGetOffset(lp);

		START_CRIT_SECTION();
		uh.ur_xact_prev = iheap_xact_last_urp(rel);
		urp = iheap_undo_write(rel, &res, &uh, payload, payload_len);

		ih_hdr_read(newtup, &nh);
		nh.xid = xid;
		nh.urec = urp;
		nh.infomask = (nh.infomask & IHT_DATA_FLAGS) | IHT_INPLACE |
			(plan.key_changed ? IHT_KEYS_UPDATED : 0) |
			(ncarried > 0 ? IHT_LOCKED : 0);
		ih_hdr_write(newtup, &nh);

		if (!relocated)
		{
			lp_off = ItemIdGetOffset(lp);
			memcpy(tup, newtup, newlen);
			ItemIdSetNormal(lp, lp_off, newlen);
			if (newlen < curlen)
				IHeapPageGetOpaque(page)->ihp_flags |= IHP_PRUNABLE;
		}
		else
		{
			if (!iheap_page_place_item_ext(page, off, newtup, newlen, capacity,
										   keep_align ?
										   ItemIdGetOffset(lp) % MAXIMUM_ALIGNOF : -1))
				elog(PANIC, "iheap: in-place update relocation failed");
			lp_off = ItemIdGetOffset(lp);
			IHeapPageGetOpaque(page)->ihp_flags |= IHP_PRUNABLE;
		}

		ih_vm_clear(rel, buf, &vm);
		MarkBufferDirty(buf);

		if (RelationNeedsWAL(rel))
		{
			xl_iheap_update xlrec;
			XLogRecPtr	recptr;
			uint32		newmid = newbody - d.prefix - d.suffix;
			char		walhdr[SizeOfIHeapWalHdr];

			xlrec.offnum = off;
			xlrec.new_len = (uint16) newlen;
			xlrec.lp_off = lp_off;
			xlrec.capacity = (uint16) (relocated ? capacity : 0);
			xlrec.prefix = d.prefix;
			xlrec.suffix = d.suffix;
			xlrec.flags = (relocated ? XLIH_RELOCATED : 0) |
				(relocated && keep_align ? XLIH_KEEP_ALIGN : 0) |
				(vm.cleared ? XLIH_VM_CLEARED : 0);
			if (logical)
			{
				/* decoding reads the new tuple from the record alone */
				xlrec.prefix = 0;
				xlrec.suffix = 0;
				newmid = newbody;
				xlrec.flags |= XLIH_LOGICAL;
			}
			Assert(UndoRecPtrGetLPage(urp) == res.lpage);
			iheap_walhdr_put(walhdr, &nh);
			XLogBeginInsert();
			XLogRegisterData(&xlrec, SizeOfIHeapUpdate);
			if (logical)
				XLogRegisterData(logpart.data, logpart.len);
			XLogRegisterBuffer(IH_BLK_DATA, buf,
							   REGBUF_STANDARD | (logical ? REGBUF_KEEP_DATA : 0));
			XLogRegisterBufData(IH_BLK_DATA, walhdr, SizeOfIHeapWalHdr);
			if (newmid > 0)
				XLogRegisterBufData(IH_BLK_DATA,
									newtup + newhs + xlrec.prefix,
									newmid);
			/* the undo delta's header comes from prefix and suffix here */
			iheap_undo_register_wal(&res, IH_BLK_UNDO,
									BufferGetBlockNumber(buf), !logical);
			if (vm.cleared)
				XLogRegisterBuffer(IH_BLK_VM, vm.vmbuf, 0);
			recptr = XLogInsert(RM_IHEAP_ID, XLOG_IHEAP_UPDATE);
			PageSetLSN(page, recptr);
			iheap_undo_set_lsn(&res, recptr);
			if (vm.cleared)
				PageSetLSN(BufferGetPage(vm.vmbuf), recptr);
		}
		END_CRIT_SECTION();

		iheap_xact_register_write(rel, urp, uh.ur_len);
		iheap_undo_release(&res);
		pfree(payload);

		*update_indexes = plan.summarized ? TU_Summarizing : TU_None;
		slot->tts_tid = *otid;
		pgstat_count_heap_update(rel, true, false);
	}
	else
	{
		/* ---- move the row to a new TID ---- */
		bool		same_page;
		bool		new_init = false;
		Page		newpage;
		OffsetNumber newoff;
		IHeapUndoRecHeader muh;
		IHeapUndoRecHeader iuh;
		IHeapUndoReservation res;
		char	   *mpayload;
		uint32		mpayload_len;
		char		ipayload[IHEAP_MAX_LOCKERS * IHEAP_LOCKER_SIZE + sizeof(uint16)];
		uint32		ipayload_len = 0;
		UndoRecPtr	murp;
		UndoRecPtr	iurp;
		IHeapHdr	oh;
		IHeapHdr	nh;
		ItemPointerData newtid;
		int			ncarried = plan.key_changed ? 0 : c.nlockers;
		uint32		curhs;
		bool		compact;
		bool		tobody;

		same_page = iheap_page_free_space(page) >= newlen + sizeof(ItemIdData) + MAXIMUM_ALIGNOF;
		if (!same_page && (IHeapPageGetOpaque(page)->ihp_flags & IHP_PRUNABLE))
		{
			(void) iheap_page_prune_opt(rel, buf);
			same_page = iheap_page_free_space(page) >= newlen + sizeof(ItemIdData) + MAXIMUM_ALIGNOF;
		}

		if (!same_page)
		{
			/*
			 * Look for the new page with the old one still locked: nobody can
			 * change the row meanwhile, so it needs no LOCK record.  A new
			 * page whose VM page would have to be pinned is given up, since
			 * pinning may read, unless the old page's VM page, pinned
			 * already, covers it.
			 */
			newbuf = ih_get_buffer(rel, newlen, NULL, buf, true, &new_init);
			if (BufferIsValid(newbuf) &&
				PageIsAllVisible(BufferGetPage(newbuf)) &&
				!BufferIsValid(newvm.vmbuf))
			{
				if (BufferIsValid(vm.vmbuf) &&
					visibilitymap_pin_ok(BufferGetBlockNumber(newbuf), vm.vmbuf))
				{
					IncrBufferRefCount(vm.vmbuf);
					newvm.vmbuf = vm.vmbuf;
				}
				else
				{
					Assert(!new_init);
					UnlockReleaseBuffer(newbuf);
					newbuf = InvalidBuffer;
				}
			}
		}

		if (same_page)
			newpage = page;
		else if (BufferIsValid(newbuf))
			newpage = BufferGetPage(newbuf);
		else
		{
			/* keep the row ours while the page lock is released */
			if (!c.we_lock || c.our_mode < mode)
				ih_add_lock(rel, buf, off, &c, mode, xid, cid);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			newbuf = iheap_get_buffer_for_tuple(rel, newlen, NULL, buf, &new_init);
			newpage = BufferGetPage(newbuf);
			if (PageIsAllVisible(newpage) && !BufferIsValid(newvm.vmbuf))
			{
				/* rare: pin the VM of the new page without locks */
				ih_unlock_pair(newbuf, buf);
				visibilitymap_pin(rel, BufferGetBlockNumber(newbuf), &newvm.vmbuf);
				ih_lock_pair(newbuf, buf);
				if (iheap_page_free_space(newpage) < newlen + sizeof(ItemIdData) + MAXIMUM_ALIGNOF)
					elog(ERROR, "iheap: lost free space on new page, retry the update");
			}
			if (PageIsAllVisible(page) && !BufferIsValid(vm.vmbuf))
				elog(ERROR, "iheap: page became all-visible while locked for update");
			ih_check_modify(rel, buf, off, cid, NULL, mode, &c);
			if (c.result != TM_Ok)
				elog(ERROR, "iheap: row changed while locked for update");
			ncarried = plan.key_changed ? 0 : c.nlockers;
		}

		lp = PageGetItemId(page, off);
		tup = (char *) page + ItemIdGetOffset(lp);
		curlen = ItemIdGetLength(lp);

		newoff = iheap_page_choose_offnum(newpage);
		if (same_page)
			ItemPointerSet(&newtid, blk, newoff);
		else
			ItemPointerSet(&newtid, BufferGetBlockNumber(newbuf), newoff);

		/*
		 * The old version's body stays on the page until no snapshot can see
		 * it any more: the MOVE record carries the new TID only.  The body
		 * goes along to undo, and the old item becomes a stub, when the old
		 * row has a compact header, which has no room for the mover's xid,
		 * and when the row leaves a page whose bodies a snapshot holds: then
		 * its room comes back when the move ends, not when the snapshot does.
		 */
		curhs = ih_hdr_size(tup);
		compact = (c.h.infomask & IHT_COMPACT) != 0;
		tobody = compact || (!same_page && iheap_page_bodies_held(rel, page));
		mpayload_len = sizeof(ItemPointerData) + (tobody ? curlen - curhs : 0);
		mpayload = palloc(mpayload_len);
		memcpy(mpayload, &newtid, sizeof(ItemPointerData));
		if (tobody)
			memcpy(mpayload + sizeof(ItemPointerData), tup + curhs, curlen - curhs);

		memset(&muh, 0, sizeof(muh));
		muh.ur_type = IHU_MOVE;
		muh.ur_flags = plan.key_changed ? IHUF_KEYS_UPDATED : 0;
		muh.ur_xid = xid;
		muh.ur_cid = cid;
		muh.ur_blkno = blk;
		muh.ur_offnum = off;
		ih_hdr_image(muh.ur_prev_hdr, tup);

		if (ncarried > 0)
		{
			ipayload_len = iheap_lockers_size(ncarried);
			iheap_lockers_serialize(ipayload, c.lockers, ncarried);
		}
		memset(&iuh, 0, sizeof(iuh));
		iuh.ur_type = IHU_INSERT;
		iuh.ur_flags = IHUF_MOVED_IN | (ncarried > 0 ? IHUF_LOCKERS : 0);
		iuh.ur_xid = xid;
		iuh.ur_cid = cid;
		iuh.ur_blkno = ItemPointerGetBlockNumber(&newtid);
		iuh.ur_offnum = newoff;

		memset(&res, 0, sizeof(res));
		iheap_undo_reserve(rel, 2 * IHEAP_UNDOREC_HDRSZ + mpayload_len + ipayload_len,
						   &res);
		if (same_page)
			ih_vm_prepare(rel, buf, &vm);
		else
			ih_vm_prepare_pair(rel, buf, &vm, newbuf, &newvm);

		lp = PageGetItemId(page, off);
		tup = (char *) page + ItemIdGetOffset(lp);

		START_CRIT_SECTION();
		muh.ur_xact_prev = iheap_xact_last_urp(rel);
		murp = iheap_undo_write(rel, &res, &muh, mpayload, mpayload_len);
		iuh.ur_xact_prev = murp;
		iurp = iheap_undo_write(rel, &res, &iuh, ipayload, ipayload_len);

		oh = c.h;
		oh.xid = xid;
		oh.urec = murp;
		oh.infomask = (c.h.infomask & IHT_DATA_FLAGS) | IHT_MOVED |
			(plan.key_changed ? IHT_KEYS_UPDATED : 0);
		if (tobody)
		{
			oh.infomask |= IHT_STUB;
			if (compact)
				oh.hoff += IHEAP_TUPHDR_SIZE - IHEAP_COMPACT_HDRSZ;
			ih_hdr_write(tup, &oh);
			ItemIdSetNormal(lp, ItemIdGetOffset(lp), IHEAP_TUPHDR_SIZE);
			iheap_page_note_room(page, xid);
		}
		else
			ih_hdr_write(tup, &oh);
		IHeapPageGetOpaque(page)->ihp_flags |= IHP_PRUNABLE;

		ih_hdr_read(newtup, &nh);
		nh.xid = xid;
		nh.urec = iurp;
		nh.infomask = (nh.infomask & IHT_DATA_FLAGS) | (ncarried > 0 ? IHT_LOCKED : 0);
		ih_hdr_write(newtup, &nh);
		if (iheap_page_add_item(newpage, newtup, newlen, newoff) != newoff)
			elog(PANIC, "iheap: failed to place moved tuple");

		ih_vm_clear(rel, buf, &vm);
		MarkBufferDirty(buf);
		if (!same_page)
		{
			ih_vm_clear(rel, newbuf, &newvm);
			MarkBufferDirty(newbuf);
		}

		if (RelationNeedsWAL(rel))
		{
			xl_iheap_move xlrec;
			XLogRecPtr	recptr;
			char		change[SizeOfIHeapWalHdrChange];
			char		walhdr[SizeOfIHeapWalHdr];
			uint32		newhs = ih_hdr_size(newtup);
			xl_iheap_move_delta mdelta = {0, 0};

			xlrec.old_offnum = off;
			xlrec.new_offnum = newoff;
			xlrec.new_len = (uint16) newlen;
			xlrec.flags = (same_page ? XLIH_SAME_PAGE : 0) |
				(new_init ? XLIH_NEW_INIT_PAGE : 0) |
				(vm.cleared ? XLIH_VM_CLEARED : 0) |
				(newvm.cleared ? XLIH_NEW_VM_CLEARED : 0) |
				(logical ? XLIH_LOGICAL : 0);
			iheap_walchange_put(change, &oh);
			iheap_walhdr_put(walhdr, &nh);

			/*
			 * On the same page, the old body stays where it was: the new one
			 * goes as the bytes that differ, as heap logs an update on the
			 * same page.  Not with a full-page image (the block's data does
			 * not go then), nor for logical decoding (it reads the new tuple
			 * from the record alone), nor when the old body went to undo (a
			 * compact header).
			 */
			if (same_page && !logical && !tobody &&
				!XLogCheckBufferNeedsBackup(buf))
			{
				ih_delta(tup + curhs, curlen - curhs, newtup + newhs,
						 newlen - newhs, &mdelta.prefix, &mdelta.suffix);
				xlrec.flags |= XLIH_MOVE_DELTA;
			}
			XLogBeginInsert();
			XLogRegisterData(&xlrec, SizeOfIHeapMove);
			if (xlrec.flags & XLIH_MOVE_DELTA)
				XLogRegisterData(&mdelta, sizeof(mdelta));
			if (logical)
				XLogRegisterData(logpart.data, logpart.len);
			/* decoding reads the new tuple even with a full-page image */
			XLogRegisterBuffer(IH_BLK_DATA, buf,
							   REGBUF_STANDARD |
							   (logical && same_page ? REGBUF_KEEP_DATA : 0));
			XLogRegisterBufData(IH_BLK_DATA, change, SizeOfIHeapWalHdrChange);
			if (same_page)
			{
				XLogRegisterBufData(IH_BLK_DATA, walhdr, SizeOfIHeapWalHdr);
				XLogRegisterBufData(IH_BLK_DATA, newtup + newhs + mdelta.prefix,
									newlen - newhs - mdelta.prefix - mdelta.suffix);
			}
			else
			{
				XLogRegisterBuffer(IH_BLK_NEWDATA, newbuf,
								   REGBUF_STANDARD | (new_init ? REGBUF_WILL_INIT : 0) |
								   (logical ? REGBUF_KEEP_DATA : 0));
				XLogRegisterBufData(IH_BLK_NEWDATA, walhdr, SizeOfIHeapWalHdr);
				XLogRegisterBufData(IH_BLK_NEWDATA, newtup + newhs, newlen - newhs);
			}
			iheap_undo_register_wal(&res, IH_BLK_UNDO,
									BufferGetBlockNumber(buf), false);
			/* a VM page shared by both pages is registered once, as VM */
			if (vm.cleared || (newvm.cleared && newvm.borrowed))
				XLogRegisterBuffer(IH_BLK_VM, vm.vmbuf, 0);
			if (newvm.cleared && !newvm.borrowed)
				XLogRegisterBuffer(IH_BLK_NEWVM, newvm.vmbuf, 0);
			recptr = XLogInsert(RM_IHEAP_ID, XLOG_IHEAP_MOVE);
			PageSetLSN(page, recptr);
			if (!same_page)
				PageSetLSN(newpage, recptr);
			iheap_undo_set_lsn(&res, recptr);
			if (vm.cleared || (newvm.cleared && newvm.borrowed))
				PageSetLSN(BufferGetPage(vm.vmbuf), recptr);
			if (newvm.cleared && !newvm.borrowed)
				PageSetLSN(BufferGetPage(newvm.vmbuf), recptr);
		}
		END_CRIT_SECTION();

		iheap_xact_register_write(rel, murp, muh.ur_len);
		iheap_xact_register_write(rel, iurp, iuh.ur_len);
		iheap_undo_release(&res);
		pfree(mpayload);

		*update_indexes = TU_All;
		slot->tts_tid = newtid;
		pgstat_count_heap_update(rel, false, !same_page);
	}

	slot->tts_tableOid = RelationGetRelid(rel);
	if (TTS_IS_IHEAPTUPLE(slot))
	{
		((IHeapTupleTableSlot *) slot)->xmin = xid;
		((IHeapTupleTableSlot *) slot)->cmin = cid;
	}

	if (BufferIsValid(newbuf))
		UnlockReleaseBuffer(newbuf);
	ih_vm_finish(&newvm);
	UnlockReleaseBuffer(buf);
	ih_vm_finish(&vm);
	if (held_tuple_lock != NoLock)
		UnlockTuple(rel, otid, held_tuple_lock);

	/*
	 * dmbtree indexes whose entry changed get their new and marked entries
	 * now that no table page is locked.  The row stays ours meanwhile.
	 */
	if (inplace && plan.dm_changed)
		dm_entries = iheap_dm_update_indexes(rel, otid, plan.modified,
											 plan.oldvalues, plan.oldisnull,
											 newvalues, newisnull);

	iheap_stat_count_update(rel,
							inplace ? IHEAP_MOVE_NONE :
							plan.inplace ? IHEAP_MOVE_SIZE : plan.move_reason,
							dm_entries);

	if (ts)
	{
		iheap_toast_end(ts);
		pfree(ts);
	}
	iheap_logical_free(&logpart);
	pfree(newmem);
	ih_plan_free(&plan);
	pfree(newvalues);
	pfree(newisnull);

	return TM_Ok;
}

/* ------------------------------------------------------------------------
 * Row locks
 * ------------------------------------------------------------------------
 */

/*
 * Does the version chain ending in header h start with the new home of a
 * row moved by "mover"?  Walks it back to its creating INSERT (frozen:
 * assume yes; the caller's snapshot keeps the mover's records from being
 * discarded).  Callers may hold the data page's lock.
 */
static bool
ih_chain_created_by(Relation rel, IHeapHdr h, TransactionId mover)
{
	bool		result = false;
	int			steps = 0;

	while (steps++ < 100000)
	{
		IHeapUndoRec *rec;

		if (iheap_is_frozen(rel, &h) || TransactionIdEquals(h.xid, mover))
		{
			result = true;
			break;
		}
		rec = iheap_undo_fetch(rel, h.urec, true);
		if (rec == NULL)
		{
			result = true;
			break;
		}
		if (rec->hdr.ur_type == IHU_INSERT || rec->hdr.ur_type == IHU_MULTI_INSERT)
		{
			result = TransactionIdEquals(rec->hdr.ur_xid, mover) &&
				(rec->hdr.ur_flags & IHUF_MOVED_IN) != 0;
			pfree(rec);
			break;
		}
		ih_hdr_read(rec->hdr.ur_prev_hdr, &h);
		pfree(rec);
	}
	return result;
}

/* Is the row at "tid" the new home of a row moved by "mover"? */
static bool
ih_tid_created_by(Relation rel, ItemPointer tid, TransactionId mover)
{
	Buffer		buf;
	Page		page;
	ItemId		lp;
	IHeapHdr	h;

	if (ItemPointerGetBlockNumber(tid) >= RelationGetNumberOfBlocks(rel))
		return false;
	buf = ReadBufferExtended(rel, MAIN_FORKNUM, ItemPointerGetBlockNumber(tid),
							 RBM_NORMAL, NULL);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	if (!IHeapPageIsData(page) ||
		ItemPointerGetOffsetNumber(tid) > PageGetMaxOffsetNumber(page))
	{
		UnlockReleaseBuffer(buf);
		return false;
	}
	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
	if (!ItemIdIsNormal(lp))
	{
		UnlockReleaseBuffer(buf);
		return false;
	}
	ih_hdr_read((char *) page + ItemIdGetOffset(lp), &h);
	UnlockReleaseBuffer(buf);
	return ih_chain_created_by(rel, h, mover);
}

/*
 * Store the newest settled version of the item (committed or ours: under a
 * running change, the one before it) in the slot; the buffer is locked.
 */
static void
ih_store_settled(Relation rel, Buffer buf, OffsetNumber off,
				 TupleTableSlot *slot)
{
	Page		page = BufferGetPage(buf);
	ItemId		lp = PageGetItemId(page, off);
	char	   *tup = (char *) page + ItemIdGetOffset(lp);
	IHeapVersion v;
	IHeapHdr	h;

	if (iheap_resolve_version_ext(rel, buf, tup, ItemIdGetLength(lp), NULL,
								  SnapshotAny, &v, true, false))
	{
		iheap_store_version(slot, &v, true);
		if (v.mem)
			pfree(v.mem);
	}
	else
	{
		ih_hdr_read(tup, &h);
		iheap_store_tuple(slot, tup, ItemIdGetLength(lp), true, h.xid,
						  InvalidTransactionId, InvalidCommandId);
	}
	ItemPointerSet(&slot->tts_tid, BufferGetBlockNumber(buf), off);
	slot->tts_tableOid = RelationGetRelid(rel);
}

/*
 * A caller that locks without following updates (ON CONFLICT DO UPDATE,
 * logical replication's apply) found the row by a search, and heap reports
 * TM_Updated once the version it found was updated, after which the caller
 * searches again.  An in-place update keeps the TID, so the versions tell:
 *
 * - When a SnapshotDirty fetch of the same command found the row (ON
 *   CONFLICT's conflict probe, apply's index search), the row changed if
 *   the version to be locked is not the one found: heap's answer.  (ON
 *   CONFLICT starts over with the same snapshot, so comparing with the
 *   snapshot would never end; apply locks under a snapshot taken after its
 *   search, which may see the newer version already.)
 * - Otherwise iheap hands the caller the newest version, unless the
 *   row's key changed since the caller's snapshot (an in-place update of a
 *   dmbtree key): the newest version may not have the key searched for.
 *   Walks the row's versions from c->h, the one to be locked.
 */
static bool
ih_probe_superseded(Relation rel, ItemPointer tid, const IHCheck *c,
					bool *have_probe)
{
	UndoRecPtr	probed = InvalidUndoRecPtr;
	IHeapHdr	h = c->h;

	*have_probe = iheap_tid_found_dirty_version(rel, tid, &probed);
	return *have_probe && iheap_version_urec(rel, &h) != probed;
}

static bool
ih_changed_since_search(Relation rel, ItemPointer tid, const IHCheck *c,
						Snapshot snapshot)
{
	bool		have_probe;
	UndoRecPtr	discard;
	IHeapHdr	h = c->h;
	int			steps = 0;

	if (ih_probe_superseded(rel, tid, c, &have_probe))
		return true;
	if (have_probe)
		return false;

	discard = iheap_discard_ptr(rel);
	for (;;)
	{
		IHeapUndoRec *rec;

		if (!IsMVCCSnapshot(snapshot) || h.urec < discard ||
			(TransactionIdIsValid(h.xid) &&
			 !XidInMVCCSnapshot(h.xid, snapshot) &&
			 iheap_xid_status(h.xid, InvalidBuffer, NULL) == IHX_COMMITTED))
			return false;		/* a version the caller's snapshot sees */
		if (h.infomask & IHT_KEYS_UPDATED)
			return true;
		if (++steps > 100000)
			break;
		rec = iheap_undo_fetch_version_rec(rel, &h);
		if (rec == NULL)
			break;
		if (rec->hdr.ur_type == IHU_INSERT ||
			rec->hdr.ur_type == IHU_MULTI_INSERT)
		{
			pfree(rec);
			break;
		}
		ih_hdr_read(rec->hdr.ur_prev_hdr, &h);
		pfree(rec);
	}
	return false;
}

TM_Result
iheap_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot,
				 TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
				 LockWaitPolicy wait_policy, uint8 flags,
				 TM_FailureData *tmfd)
{
	bool		follow = (flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION) != 0;

	/*
	 * As heap: KEY SHARE is not disturbed by committed changes that left the
	 * key alone (in-place updates, moves without key change): it locks the
	 * newest version without asking for a recheck, at any isolation level.
	 */
	bool		nokey_chain = (mode == LockTupleKeyShare);
	TransactionId nokey_mover = InvalidTransactionId;
	bool		slot_filled = false;
	LOCKMODE	held_tuple_lock = NoLock;
	TransactionId xid;
	ItemPointerData locked_tid = *tid;
	TM_Result	result;

	tmfd->traversed = false;
	xid = GetCurrentTransactionId();

	for (;;)
	{
		BlockNumber blk = ItemPointerGetBlockNumber(tid);
		OffsetNumber off = ItemPointerGetOffsetNumber(tid);
		Buffer		buf;
		Page		page;
		IHCheck		c;
		bool		retry_other = false;

		buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		if (!IHeapPageIsData(page))
		{
			UnlockReleaseBuffer(buf);
			elog(ERROR, "iheap: tid (%u,%u) does not point to a data page", blk, off);
		}

		for (;;)
		{
			ih_check_modify_ext(rel, buf, off, cid, snapshot, mode, nokey_mover,
								&c);
			if (c.result != TM_BeingModified)
				break;
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			if (!ih_wait(rel, tid, c.xwait, mode, wait_policy, c.we_lock,
						 &held_tuple_lock, XLTW_Lock))
			{
				ReleaseBuffer(buf);
				ih_fill_tmfd(tmfd, &c);
				if (held_tuple_lock != NoLock)
					UnlockTuple(rel, &locked_tid, held_tuple_lock);
				return TM_WouldBlock;
			}
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		}

		result = c.result;
		/* a change of the key (or a move elsewhere) ends the exemption */
		if (result == TM_Updated && (c.h.infomask & IHT_KEYS_UPDATED) != 0)
			nokey_chain = false;

		if (result == TM_Updated && follow &&
			ItemPointerIndicatesMovedPartitions(&c.ctid))
		{
			/* as heap: the newest version is nowhere to be locked */
			UnlockReleaseBuffer(buf);
			if (held_tuple_lock != NoLock)
				UnlockTuple(rel, &locked_tid, held_tuple_lock);
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("tuple to be locked was already moved to another partition due to concurrent update")));
		}

		if (c.running_move)
		{
			/*
			 * KEY SHARE over a running update that moved the row and kept
			 * its key: as heap, no waiting.  Lock this old version (should
			 * the update roll back, the lock passes to the restored
			 * version), then the new one (should it commit).  The caller
			 * gets the old version, as from heap.
			 */
			TransactionId mover = c.modxid;
			ItemPointerData next = c.ctid;

			if (!(c.we_locked_version && c.our_mode >= mode))
				ih_add_lock(rel, buf, off, &c, mode, xid, cid);
			ih_store_settled(rel, buf, off, slot);
			slot_filled = true;
			UnlockReleaseBuffer(buf);
			if (held_tuple_lock != NoLock)
			{
				UnlockTuple(rel, &locked_tid, held_tuple_lock);
				held_tuple_lock = NoLock;
			}
			if (!ih_tid_created_by(rel, &next, mover))
				return TM_Ok;	/* rolled back already: the old one is it */
			*tid = next;
			locked_tid = next;
			nokey_mover = mover;
			continue;
		}

		/*
		 * Following a running non-key move, the new home is gone (its slot
		 * possibly reused already): the move rolled back, and the lock taken
		 * on the old version holds.
		 */
		if (TransactionIdIsValid(nokey_mover) &&
			(result == TM_Deleted ? TransactionIdDidAbort(nokey_mover) :
			 !ih_chain_created_by(rel, c.h, nokey_mover)))
		{
			UnlockReleaseBuffer(buf);
			if (held_tuple_lock != NoLock)
				UnlockTuple(rel, &locked_tid, held_tuple_lock);
			return TM_Ok;
		}

		if (result == TM_Updated && (follow || nokey_chain) &&
			!ItemPointerEquals(&c.ctid, tid) &&
			!ItemPointerIndicatesMovedPartitions(&c.ctid))
		{
			/* the row moved: follow it, if the target is its new home */
			TransactionId mover = c.modxid;
			ItemPointerData next = c.ctid;

			UnlockReleaseBuffer(buf);
			if (!ih_tid_created_by(rel, &next, mover))
			{
				if (held_tuple_lock != NoLock)
					UnlockTuple(rel, &locked_tid, held_tuple_lock);
				ih_fill_tmfd(tmfd, &c);
				return TM_Deleted;
			}
			/* the heavyweight lock is for the TID we wait on: drop it */
			if (held_tuple_lock != NoLock)
			{
				UnlockTuple(rel, &locked_tid, held_tuple_lock);
				held_tuple_lock = NoLock;
			}
			*tid = next;
			locked_tid = next;
			if (!nokey_chain)
				tmfd->traversed = true;
			retry_other = true;
		}
		else if (result == TM_Ok && c.newer)
		{
			/*
			 * A version newer than the snapshot: as heap following the
			 * update chain, the caller must recheck it.  Not again, though,
			 * once it was handed to a modifying caller (ih_grant_version):
			 * that one rechecked it already, and would retry forever.
			 */
			if (nokey_chain && (c.h.infomask & IHT_KEYS_UPDATED) == 0)
				;				/* KEY SHARE, key unchanged: nothing to recheck */
			else if ((flags & TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS) == 0 &&
					 ih_version_granted(rel, tid, &c, xid, cid))
				;
			else if (follow)
				tmfd->traversed = true;
			else if (IsolationUsesXactSnapshot() ||
					 ih_changed_since_search(rel, tid, &c, snapshot))
			{
				result = TM_Updated;
				c.modxid = c.h.xid;
			}
		}
		else if (result == TM_Ok && !follow &&
				 (flags & TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS) == 0)
		{
			bool		have_probe;

			/* the snapshot sees it, but the caller's search found an older one */
			if (ih_probe_superseded(rel, tid, &c, &have_probe))
			{
				result = TM_Updated;
				c.modxid = c.h.xid;
			}
		}

		if (retry_other)
			continue;

		/*
		 * A conflict probe (SnapshotDirty) found a row the current command
		 * updated in place, and ON CONFLICT now locks it: in heap the probe
		 * finds the new version, which the command inserted, so heap says
		 * TM_Invisible ("cannot affect row a second time").
		 */
		if (result == TM_SelfModified &&
			(flags & TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS) == 0 &&
			iheap_tid_found_dirty(rel, tid))
			result = TM_Invisible;

		if (result != TM_Ok)
		{
			ItemId		lp = PageGetItemId(page, off);

			/*
			 * As heap, leave the row in the slot: callers look at it (ON
			 * CONFLICT reads xmin to tell a row inserted by the current
			 * command from a concurrent one).
			 */
			if (off <= PageGetMaxOffsetNumber(page) && ItemIdIsNormal(lp) &&
				(ih_hdr_infomask((char *) page + ItemIdGetOffset(lp)) & IHT_STUB) == 0)
			{
				iheap_store_tuple(slot, (char *) page + ItemIdGetOffset(lp),
								  ItemIdGetLength(lp), true, c.h.xid,
								  InvalidTransactionId, InvalidCommandId);
				ItemPointerSet(&slot->tts_tid, blk, off);
				slot->tts_tableOid = RelationGetRelid(rel);
			}
			ih_fill_tmfd(tmfd, &c);
			UnlockReleaseBuffer(buf);
			if (held_tuple_lock != NoLock)
				UnlockTuple(rel, &locked_tid, held_tuple_lock);
			return result;
		}

		/*
		 * Take the lock unless we already hold one at least as strong on
		 * this very version (a lock carried in from an older version does not
		 * count: the caller must see that it now holds the newest one).
		 */
		if (!(c.status == IHX_CURRENT && !c.frozen) &&
			!(c.we_locked_version && c.our_mode >= mode))
			ih_add_lock(rel, buf, off, &c, mode, xid, cid);
		if ((flags & TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS) == 0)
			ih_grant_version(rel, tid, &c, xid, cid);

		/*
		 * Return the current version (the last settled one, under a running
		 * update), unless the old home of a moving row was returned already.
		 */
		if (!slot_filled)
		{
			if (c.under_running_update)
				ih_store_settled(rel, buf, off, slot);
			else
			{
				ItemId		lp = PageGetItemId(page, off);

				iheap_store_tuple(slot, (char *) page + ItemIdGetOffset(lp),
								  ItemIdGetLength(lp), true, c.h.xid,
								  InvalidTransactionId, InvalidCommandId);
				ItemPointerSet(&slot->tts_tid, blk, off);
				slot->tts_tableOid = RelationGetRelid(rel);
			}
		}

		UnlockReleaseBuffer(buf);
		if (held_tuple_lock != NoLock)
			UnlockTuple(rel, &locked_tid, held_tuple_lock);
		return TM_Ok;
	}
}
