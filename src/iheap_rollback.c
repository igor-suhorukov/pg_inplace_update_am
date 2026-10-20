/*-------------------------------------------------------------------------
 *
 * iheap_rollback.c
 *	  Applying undo of aborted transactions.
 *
 * Rollback has three layers (DESIGN.md §8, §21 D8):
 *
 *	1. Readers skip versions of aborted transactions by walking undo.
 *	2. Writers (and pruning, and discard) roll back an item whose current
 *	   version belongs to an aborted transaction before touching it.
 *	3. As an optimization, the aborting backend applies its own undo in the
 *	   transaction/subtransaction abort callbacks.
 *
 * Only 1 and 2 are needed for correctness: the abort callbacks run after the
 * abort is already visible to others (xact.c), may be skipped (crash,
 * failure, prepared transactions), and are bounded by
 * iheap.foreground_undo_limit.
 *
 * The work of 3 is not done in the abort callbacks themselves: an error can
 * leave buffer content locks held, and those are released only when the
 * transaction's resource owner is (after the callbacks).  The callbacks
 * mark the rollback pending; a resource release callback does it in the
 * LOCKS phase, when all buffer pins and content locks are gone.
 *
 * Applying a record is idempotent: an item is rolled back only while its
 * current version's creator is aborted, and each step restores exactly the
 * previous version.
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/pg_class.h"
#include "catalog/storage.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner.h"

#include "iheap.h"
#include "iheap_xlog.h"

static void
log_rollback(Relation rel, Buffer buffer, OffsetNumber off, uint8 action,
			 uint16 lp_off, const char *data, uint16 len)
{
	xl_iheap_rollback xlrec;
	XLogRecPtr	recptr;

	if (!RelationNeedsWAL(rel))
		return;

	xlrec.offnum = off;
	xlrec.action = action;
	xlrec.lp_off = lp_off;
	xlrec.len = len;

	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfIHeapRollback);
	XLogRegisterBuffer(IH_BLK_DATA, buffer, REGBUF_STANDARD);
	if (len > 0)
		XLogRegisterBufData(IH_BLK_DATA, data, len);
	recptr = XLogInsert(RM_IHEAP_ID, XLOG_IHEAP_ROLLBACK);
	PageSetLSN(BufferGetPage(buffer), recptr);
}

/* bytes available at the item's location before the next tuple */
static uint32
item_capacity(Page page, OffsetNumber off)
{
	PageHeader	ph = (PageHeader) page;
	ItemId		lp = PageGetItemId(page, off);
	uint32		start = ItemIdGetOffset(lp);
	uint32		limit = ph->pd_special;
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

	for (OffsetNumber i = FirstOffsetNumber; i <= maxoff; i++)
	{
		ItemId		other = PageGetItemId(page, i);
		uint32		o;

		if (i == off || !ItemIdHasStorage(other))
			continue;
		o = ItemIdGetOffset(other);
		if (o > start && o < limit)
			limit = o;
	}
	return limit - start;
}

/*
 * Roll back the item at "off" while its current version was created by an
 * aborted transaction.  The caller holds an exclusive lock on the buffer.
 * Returns true if anything changed.
 */
bool
iheap_rollback_tuple(Relation rel, Buffer buffer, OffsetNumber off)
{
	Page		page = BufferGetPage(buffer);
	bool		changed = false;
	int			steps = 0;
	IHeapLocker keep[IHEAP_MAX_LOCKERS];
	int			nkeep = 0;

	for (;;)
	{
		ItemId		lp;
		char	   *tup;
		IHeapHdr	h;
		IHeapUndoRec *rec;

		if (off < FirstOffsetNumber || off > PageGetMaxOffsetNumber(page))
			break;
		lp = PageGetItemId(page, off);
		if (!ItemIdIsNormal(lp))
			break;
		tup = (char *) page + ItemIdGetOffset(lp);
		ih_hdr_read(tup, &h);
		if (h.urec < iheap_discard_ptr(rel))
			break;
		if (iheap_xid_status(h.xid, buffer, tup) != IHX_ABORTED)
			break;

		if (++steps > 100000)
			elog(ERROR, "iheap: rollback chain too long in relation \"%s\"",
				 RelationGetRelationName(rel));

		rec = iheap_undo_fetch(rel, h.urec, false);

		switch (rec->hdr.ur_type)
		{
			case IHU_INSERT:
			case IHU_MULTI_INSERT:
				START_CRIT_SECTION();
				ItemIdSetDead(lp);
				IHeapPageGetOpaque(page)->ihp_flags |= IHP_PRUNABLE;
				MarkBufferDirty(buffer);
				log_rollback(rel, buffer, off, XLIH_RB_KILL, 0, NULL, 0);
				END_CRIT_SECTION();
				pfree(rec);
				return true;

			case IHU_LOCK:
				{
					IHeapLocker lk[IHEAP_MAX_LOCKERS];
					int			n = iheap_undo_lockers(rec, lk);

					/*
					 * Locks taken on top of the aborted version by others
					 * that still run must survive it (see below).
					 */
					for (int i = 0; i < n; i++)
					{
						int			j;

						if (!TransactionIdIsInProgress(lk[i].xid))
							continue;
						for (j = 0; j < nkeep; j++)
							if (TransactionIdEquals(keep[j].xid, lk[i].xid))
								break;
						if (j == nkeep && nkeep < IHEAP_MAX_LOCKERS)
							keep[nkeep++] = lk[i];
						else if (j < nkeep)
							keep[j].mode = Max(keep[j].mode, lk[i].mode);
					}
				}
				pg_fallthrough;
			case IHU_DELETE:
			case IHU_MOVE:
				/* the body is still on the page: restore the header */
				START_CRIT_SECTION();
				memcpy(tup, rec->hdr.ur_prev_hdr, IHEAP_TUPHDR_SIZE);
				MarkBufferDirty(buffer);
				log_rollback(rel, buffer, off, XLIH_RB_HEADER,
							 ItemIdGetOffset(lp), tup, IHEAP_TUPHDR_SIZE);
				END_CRIT_SECTION();
				break;

			case IHU_UPDATE:
				{
					char	   *old = tup;
					uint32		oldlen = ItemIdGetLength(lp);
					char	   *mem;
					IHeapHdr	oh;
					uint32		cap;
					uint16		lp_off = ItemIdGetOffset(lp);

					if (!iheap_rebuild_prior(rel, &old, &oldlen, &oh, rec, &mem))
						elog(ERROR, "iheap: cannot rebuild previous version");

					cap = item_capacity(page, off);
					if (oldlen > cap)
					{
						/* reservation was lost: use free space */
						PageHeader	ph = (PageHeader) page;

						if ((int) ph->pd_upper - (int) ph->pd_lower < (int) oldlen + MAXIMUM_ALIGNOF)
							elog(ERROR, "iheap: no room to roll back an update in relation \"%s\"",
								 RelationGetRelationName(rel));
						START_CRIT_SECTION();
						if (!iheap_page_place_item(page, off, old, oldlen))
							elog(PANIC, "iheap: rollback placement failed");
						lp_off = ItemIdGetOffset(lp);
					}
					else
					{
						START_CRIT_SECTION();
						memcpy(tup, old, oldlen);
						ItemIdSetNormal(lp, lp_off, oldlen);
					}
					MarkBufferDirty(buffer);
					log_rollback(rel, buffer, off, XLIH_RB_TUPLE, lp_off,
								 (char *) page + lp_off, (uint16) oldlen);
					END_CRIT_SECTION();
					pfree(mem);
					break;
				}

			default:
				elog(ERROR, "iheap: unknown undo record type %u", rec->hdr.ur_type);
		}

		pfree(rec);
		changed = true;
	}

	/* hand surviving locks to the restored version */
	if (nkeep > 0)
		iheap_relock_after_rollback(rel, buffer, off, keep, nkeep);

	return changed;
}

/* roll back every item of the page created by an aborted transaction */
int
iheap_rollback_page(Relation rel, Buffer buffer)
{
	Page		page = BufferGetPage(buffer);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	UndoRecPtr	discard = iheap_discard_ptr(rel);
	int			n = 0;

	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		lp = PageGetItemId(page, off);
		IHeapHdr	h;
		char	   *tup;

		if (!ItemIdIsNormal(lp))
			continue;
		tup = (char *) page + ItemIdGetOffset(lp);
		ih_hdr_read(tup, &h);
		if (h.urec < discard)
			continue;
		if (iheap_xid_status(h.xid, buffer, tup) != IHX_ABORTED)
			continue;
		if (iheap_rollback_tuple(rel, buffer, off))
			n++;
	}
	return n;
}

/* ------------------------------------------------------------------------
 * Eager rollback by the aborting backend
 * ------------------------------------------------------------------------
 */
typedef struct XactRelEnt
{
	RelFileLocator key;
	char		persistence;
	bool		needs_wal;
	UndoRecPtr	last_urp;		/* latest record of this transaction */
	Size		bytes;			/* undo written by this transaction */
} XactRelEnt;

static HTAB *xact_rels = NULL;
static MemoryContext rollback_cxt = NULL;
static uint32 xacts_since_cache_reset = 0;

/* eager rollback waiting for the resource owner release (see above) */
static bool pending_top_rollback = false;
static bool pending_sub_rollback = false;

static XactRelEnt *
xact_rel_entry(Relation rel, bool create)
{
	bool		found;
	XactRelEnt *ent;

	if (xact_rels == NULL)
	{
		HASHCTL		ctl;

		if (!create)
			return NULL;
		ctl.keysize = sizeof(RelFileLocator);
		ctl.entrysize = sizeof(XactRelEnt);
		ctl.hcxt = TopMemoryContext;
		xact_rels = hash_create("iheap transaction relations", 16, &ctl,
								HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	ent = hash_search(xact_rels, &rel->rd_locator,
					  create ? HASH_ENTER : HASH_FIND, &found);
	if (create && !found)
	{
		ent->persistence = rel->rd_rel->relpersistence;
		ent->needs_wal = RelationNeedsWAL(rel);
		ent->last_urp = InvalidUndoRecPtr;
		ent->bytes = 0;
	}
	return ent;
}

/* remember that the current transaction wrote undo record urp in rel */
void
iheap_xact_register_write(Relation rel, UndoRecPtr urp, Size len)
{
	XactRelEnt *ent = xact_rel_entry(rel, true);

	ent->last_urp = urp;
	ent->bytes += len;
}

UndoRecPtr
iheap_xact_last_urp(Relation rel)
{
	XactRelEnt *ent = xact_rel_entry(rel, false);

	return ent ? ent->last_urp : InvalidUndoRecPtr;
}

/*
 * The relation's storage was truncated in place (it was created in the
 * current subtransaction, so aborting drops it): the records written so far
 * no longer exist.
 */
void
iheap_xact_forget_relation(RelFileLocator rlocator)
{
	if (xact_rels != NULL)
		(void) hash_search(xact_rels, &rlocator, HASH_REMOVE, NULL);
}

static void
rollback_record_targets(Relation rel, IHeapUndoRec *rec)
{
	Buffer		buf;
	OffsetNumber offs[MaxOffsetNumber];
	int			noffs = 0;

	if (rec->hdr.ur_type == IHU_MULTI_INSERT)
	{
		uint16		n;

		memcpy(&n, rec->payload, sizeof(uint16));
		for (int i = 0; i < n && i < MaxOffsetNumber; i++)
			memcpy(&offs[noffs++], rec->payload + sizeof(uint16) + i * sizeof(OffsetNumber),
				   sizeof(OffsetNumber));
	}
	else
		offs[noffs++] = rec->hdr.ur_offnum;

	if (rec->hdr.ur_blkno >= RelationGetNumberOfBlocks(rel))
		return;

	buf = ReadBufferExtended(rel, MAIN_FORKNUM, rec->hdr.ur_blkno, RBM_NORMAL,
							 NULL);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	if (IHeapPageIsData(BufferGetPage(buf)))
	{
		for (int i = 0; i < noffs; i++)
			(void) iheap_rollback_tuple(rel, buf, offs[i]);
	}
	UnlockReleaseBuffer(buf);
}

/*
 * Apply this backend's undo in one relation, newest record first.  With
 * subxact, stop at the first record whose writer did not abort (records of
 * the aborted subtransaction tree are always the newest ones).
 */
static void
rollback_relation(XactRelEnt *ent, bool subxact)
{
	Relation	rel;
	UndoRecPtr	urp = ent->last_urp;
	MemoryContext oldcxt;

	if (!UndoRecPtrIsValid(urp))
		return;

	rel = iheap_fake_relation_open(ent->key, ent->persistence);

	oldcxt = MemoryContextSwitchTo(rollback_cxt);

	while (UndoRecPtrIsValid(urp))
	{
		IHeapUndoRec *rec;

		CHECK_FOR_INTERRUPTS();
		rec = iheap_undo_fetch(rel, urp, true);
		if (rec == NULL)
			break;
		if (subxact && !TransactionIdDidAbort(rec->hdr.ur_xid))
			break;
		rollback_record_targets(rel, rec);
		urp = rec->hdr.ur_xact_prev;
		MemoryContextReset(rollback_cxt);
	}
	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(rollback_cxt);

	ent->last_urp = subxact ? urp : InvalidUndoRecPtr;

	iheap_fake_relation_close(rel);
}

static void
rollback_all(bool subxact)
{
	HASH_SEQ_STATUS status;
	XactRelEnt *ent;
	RelFileLocator *dropped;
	int			ndropped;
	int			i;
	uint32		save_holdoff;
	uint32		save_qc_holdoff;
	ResourceOwner owner;
	ResourceOwner save_owner;

	if (xact_rels == NULL)
		return;

	/* storage created by the aborting (sub)transaction is about to go away */
	ndropped = smgrGetPendingDeletes(false, &dropped);

	if (rollback_cxt == NULL)
		rollback_cxt = AllocSetContextCreate(TopMemoryContext,
											 "iheap rollback",
											 ALLOCSET_DEFAULT_SIZES);

	hash_seq_init(&status, xact_rels);
	while ((ent = hash_seq_search(&status)) != NULL)
	{
		/* temp relations live in local buffers: left to lazy rollback */
		if (ent->persistence == RELPERSISTENCE_TEMP)
			continue;
		/* WAL-skipped (created in this transaction, wal_level=minimal) */
		if (ent->persistence == RELPERSISTENCE_PERMANENT && !ent->needs_wal)
			continue;
		for (i = 0; i < ndropped; i++)
		{
			if (RelFileLocatorEquals(dropped[i], ent->key))
				break;
		}
		if (i < ndropped)
		{
			/* gone with the (sub)transaction: forget it */
			(void) hash_search(xact_rels, &ent->key, HASH_REMOVE, NULL);
			continue;
		}
		if (ent->bytes > (Size) iheap_foreground_undo_limit * 1024)
			continue;

		/*
		 * The transaction's resource owners are being released, so buffer
		 * pins go to an owner of our own, released (with whatever an error
		 * left pinned or locked) after each relation.  Errors are caught:
		 * the abort must go on.  errfinish() zeroes the interrupt holdoff
		 * counts, which the aborting code relies on.
		 */
		owner = ResourceOwnerCreate(NULL, "iheap rollback");
		save_owner = CurrentResourceOwner;
		CurrentResourceOwner = owner;
		save_holdoff = InterruptHoldoffCount;
		save_qc_holdoff = QueryCancelHoldoffCount;
		PG_TRY();
		{
			rollback_relation(ent, subxact);
		}
		PG_CATCH();
		{
			ErrorData  *edata;

			InterruptHoldoffCount = save_holdoff;
			QueryCancelHoldoffCount = save_qc_holdoff;
			MemoryContextSwitchTo(TopMemoryContext);
			edata = CopyErrorData();
			FlushErrorState();
			ereport(LOG,
					(errmsg("iheap: could not apply undo at abort, leaving it to lazy rollback: %s",
							edata->message)));
			FreeErrorData(edata);
			MemoryContextReset(rollback_cxt);
		}
		PG_END_TRY();
		/* a standalone owner (no parent) is released as a top-level one */
		CurrentResourceOwner = save_owner;
		ResourceOwnerRelease(owner, RESOURCE_RELEASE_BEFORE_LOCKS, false, true);
		ResourceOwnerRelease(owner, RESOURCE_RELEASE_LOCKS, false, true);
		ResourceOwnerRelease(owner, RESOURCE_RELEASE_AFTER_LOCKS, false, true);
		ResourceOwnerDelete(owner);
	}

	if (ndropped > 0)
		pfree(dropped);
}

static void
xact_cleanup(void)
{
	if (xact_rels)
	{
		hash_destroy(xact_rels);
		xact_rels = NULL;
	}
	if (++xacts_since_cache_reset >= 100000)
	{
		iheap_xid_cache_reset();
		xacts_since_cache_reset = 0;
	}
}

static void
iheap_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_ABORT:
			if (xact_rels != NULL)
				pending_top_rollback = true;
			else
				xact_cleanup();
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_PREPARE:
			pending_top_rollback = pending_sub_rollback = false;
			xact_cleanup();
			break;
		default:
			break;
	}
}

static void
iheap_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
					   SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_ABORT_SUB && xact_rels != NULL)
		pending_sub_rollback = true;
}

/*
 * The aborting (sub)transaction's resource owner tree is being released.
 * By the LOCKS phase every buffer pin and content lock is gone (they go in
 * the BEFORE_LOCKS phase, the whole tree first), so undo can be applied.
 */
static void
iheap_resowner_callback(ResourceReleasePhase phase, bool isCommit,
						bool isTopLevel, void *arg)
{
	if (phase != RESOURCE_RELEASE_LOCKS || isCommit)
		return;

	if (pending_top_rollback)
	{
		pending_top_rollback = pending_sub_rollback = false;
		rollback_all(false);
		xact_cleanup();
	}
	else if (pending_sub_rollback)
	{
		pending_sub_rollback = false;
		rollback_all(true);
	}
}

void
iheap_register_xact_callbacks(void)
{
	RegisterXactCallback(iheap_xact_callback, NULL);
	RegisterSubXactCallback(iheap_subxact_callback, NULL);
	RegisterResourceReleaseCallback(iheap_resowner_callback, NULL);
}
