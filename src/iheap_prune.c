/*-------------------------------------------------------------------------
 *
 * iheap_prune.c
 *	  Data page space management: item placement, compaction, pruning.
 *
 * Space rules (DESIGN.md §21 D6, D40):
 *	- Storage freed by a shrinking in-place UPDATE stays reserved until the
 *	  writer commits, because rollback restores the old version in place.
 *	- A deleted or moved row keeps its body on the page until every
 *	  snapshot sees the deletion, as heap's does: old snapshots read it there.
 *	  Then it shrinks to a 16-byte stub, and the prune record carries the
 *	  deleter as conflict horizon for standbys.  Once the stub is frozen it
 *	  becomes LP_DEAD (no storage) until index vacuum frees the line pointer.
 *	  Until the horizon passes, pd_prune_xid holds what a prune waits for,
 *	  and writers looking for room do not prune before (as heap).
 *	- A row with a compact header that is deleted or moved, and a row that
 *	  moves off a page whose bodies a snapshot holds (§21 D45), becomes
 *	  a stub at once, its body in undo.  Its storage stays reserved until
 *	  the writer ends; ihp_room_xid names the newest such writer, and once
 *	  it has ended, writers looking for room compact the page without
 *	  waiting for the horizon.
 *	- Compaction needs only an exclusive content lock: readers never touch
 *	  page memory without holding the content lock (§21 D11).
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
#include "storage/procarray.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "iheap.h"
#include "iheap_xlog.h"

/* free space for one more tuple (including a new line pointer if needed) */
Size
iheap_page_free_space(Page page)
{
	PageHeader	ph = (PageHeader) page;
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	int			space = (int) ph->pd_upper - (int) ph->pd_lower;

	if (!PageHasFreeLinePointers(page))
	{
		if (maxoff >= IHEAP_MAX_ITEMS)
			return 0;
		space -= sizeof(ItemIdData);
	}
	return space > 0 ? (Size) space : 0;
}

/* offset number iheap_page_add_item() will pick when not told one */
OffsetNumber
iheap_page_choose_offnum(Page page)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

	if (PageHasFreeLinePointers(page))
	{
		for (OffsetNumber i = FirstOffsetNumber; i <= maxoff; i++)
			if (!ItemIdIsUsed(PageGetItemId(page, i)))
				return i;
	}
	return maxoff + 1;
}

/* start offset for a tuple of "len" bytes ending at or below "upper" */
static inline int
ih_place_start(int upper, const char *tuple, uint32 len)
{
	int			start = upper - (int) len;

	if (ih_tuple_needs_align(tuple))
	{
		int			hoff = ih_hdr_hoff(tuple);

		start = ((start + hoff) & ~(MAXIMUM_ALIGNOF - 1)) - hoff;
	}
	else
		start &= ~1;
	return start;
}

/*
 * Is page offset "start" a place for "tuple"?  A tuple holding values that
 * need alignment (IHT_ALIGNDATA) needs its data area MAXALIGNed there; the
 * others can be anywhere, their fields being read with memcpy.
 */
bool
iheap_tuple_aligned_at(const char *tuple, uint32 start)
{
	return !ih_tuple_needs_align(tuple) ||
		(start + ih_hdr_hoff(tuple)) % MAXIMUM_ALIGNOF == 0;
}

/*
 * Does every place aligned for "tuple" suit "other" too?  A rollback puts
 * the version an in-place update replaced (other) back where the new one
 * (tuple) went.
 */
bool
iheap_tuple_alignment_covers(const char *tuple, const char *other)
{
	if (!ih_tuple_needs_align(other))
		return true;
	return ih_tuple_needs_align(tuple) &&
		ih_hdr_hoff(tuple) % MAXIMUM_ALIGNOF == ih_hdr_hoff(other) % MAXIMUM_ALIGNOF;
}

/*
 * Add a tuple to the page.  "want" is the offset number to use (redo), or
 * InvalidOffsetNumber to pick one.  Returns InvalidOffsetNumber if it does
 * not fit.
 */
OffsetNumber
iheap_page_add_item(Page page, const char *tuple, uint32 len, OffsetNumber want)
{
	PageHeader	ph = (PageHeader) page;
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	OffsetNumber off = InvalidOffsetNumber;
	bool		newlp;
	int			lower;
	int			start;
	ItemId		lp;

	if (OffsetNumberIsValid(want))
	{
		off = want;
		if (off > maxoff + 1)
			return InvalidOffsetNumber;
		if (off <= maxoff && ItemIdIsUsed(PageGetItemId(page, off)))
			return InvalidOffsetNumber;
	}
	else
	{
		if (PageHasFreeLinePointers(page))
		{
			for (OffsetNumber i = FirstOffsetNumber; i <= maxoff; i++)
			{
				if (!ItemIdIsUsed(PageGetItemId(page, i)))
				{
					off = i;
					break;
				}
			}
			if (!OffsetNumberIsValid(off))
				PageClearHasFreeLinePointers(page);
		}
		if (!OffsetNumberIsValid(off))
			off = maxoff + 1;
	}

	if (off > IHEAP_MAX_ITEMS)
		return InvalidOffsetNumber;

	newlp = (off > maxoff);
	lower = ph->pd_lower + (newlp ? sizeof(ItemIdData) : 0);
	start = ih_place_start(ph->pd_upper, tuple, len);
	if (start < lower)
		return InvalidOffsetNumber;

	memcpy((char *) page + start, tuple, len);
	lp = PageGetItemId(page, off);
	ItemIdSetNormal(lp, start, len);
	ph->pd_upper = start;
	if (newlp)
		ph->pd_lower += sizeof(ItemIdData);
	else
	{
		/* recompute the free-line-pointer hint */
		bool		anyfree = false;

		for (OffsetNumber i = FirstOffsetNumber; i <= PageGetMaxOffsetNumber(page); i++)
		{
			if (!ItemIdIsUsed(PageGetItemId(page, i)))
			{
				anyfree = true;
				break;
			}
		}
		if (anyfree)
			PageSetHasFreeLinePointers(page);
		else
			PageClearHasFreeLinePointers(page);
	}
	return off;
}

/*
 * Give an existing item new storage of "len" bytes (plus "capacity - len"
 * reserved bytes after it) taken from the page's free space, and copy the
 * tuple there.  Returns false if there is not enough free space.
 */
bool
iheap_page_place_item(Page page, OffsetNumber off, const char *tuple,
					  uint32 len)
{
	return iheap_page_place_item_ext(page, off, tuple, len, len);
}

bool
iheap_page_place_item_ext(Page page, OffsetNumber off, const char *tuple,
						  uint32 len, uint32 capacity)
{
	PageHeader	ph = (PageHeader) page;
	ItemId		lp = PageGetItemId(page, off);
	int			start;

	Assert(capacity >= len);
	start = ih_place_start(ph->pd_upper - (int) (capacity - len), tuple, len);
	if (start < ph->pd_lower)
		return false;

	memcpy((char *) page + start, tuple, len);
	if (capacity > len)
		memset((char *) page + start + len, 0, capacity - len);
	ItemIdSetNormal(lp, start, len);
	ph->pd_upper = start;
	return true;
}

/*
 * Reservation needed by one item: the largest old length among the
 * uncommitted in-place updates in its chain, and the length of a version
 * an uncommitted deletion or move put in undo (a compact row's, or one
 * that moved off a page whose bodies a snapshot holds, whose item became a
 * stub at once), 0 if none.  Rollback of those changes restores the old,
 * longer tuple at the item's location.
 */
uint32
iheap_item_reservation(Relation rel, Page page, OffsetNumber off)
{
	ItemId		lp = PageGetItemId(page, off);
	char	   *tup;
	IHeapHdr	h;
	UndoRecPtr	urp;
	UndoRecPtr	discard;
	uint32		reserve = 0;
	int			steps = 0;

	if (!ItemIdIsNormal(lp))
		return 0;

	tup = (char *) page + ItemIdGetOffset(lp);
	ih_hdr_read(tup, &h);
	discard = iheap_discard_ptr(rel);
	if (h.urec < discard)
		return 0;

	/*
	 * The tuple's hint first: while a snapshot holds discard back, every
	 * prune asks this of each item changed since, mostly by writers long
	 * committed (§21 D45).
	 */
	if (iheap_xid_status(h.xid, InvalidBuffer, tup) == IHX_COMMITTED)
		return 0;

	urp = h.urec;
	while (UndoRecPtrIsValid(urp) && urp >= discard && steps++ < 100000)
	{
		IHeapUndoRec *rec = iheap_undo_fetch(rel, urp, true);
		IHeapHdr	ph;

		if (rec == NULL)
			break;
		ih_hdr_read(rec->hdr.ur_prev_hdr, &ph);

		if (rec->hdr.ur_type == IHU_LOCK)
		{
			urp = ph.urec;
			pfree(rec);
			continue;
		}
		if (iheap_xid_status(rec->hdr.ur_xid, InvalidBuffer, NULL) == IHX_COMMITTED)
		{
			pfree(rec);
			break;
		}
		if (rec->hdr.ur_type == IHU_UPDATE)
		{
			IHeapUpdateDelta d;

			memcpy(&d, rec->payload, sizeof(d));
			reserve = Max(reserve, d.old_len);
		}
		else if (rec->hdr.ur_type == IHU_DELETE || rec->hdr.ur_type == IHU_MOVE)
		{
			uint32		skip = (rec->hdr.ur_type == IHU_MOVE) ?
				sizeof(ItemPointerData) : 0;
			uint32		core = iheap_undo_payload_core_len(rec);

			if (core > skip)
				reserve = Max(reserve, ih_hdr_size_of(&ph) + core - skip);
		}
		if (rec->hdr.ur_type == IHU_INSERT ||
			rec->hdr.ur_type == IHU_MULTI_INSERT)
		{
			pfree(rec);
			break;
		}
		urp = ph.urec;
		pfree(rec);
	}

	return reserve > ItemIdGetLength(lp) ? reserve : 0;
}

void
iheap_compute_reservations(Relation rel, Page page, uint32 *reserve)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
		reserve[off] = iheap_item_reservation(rel, page, off);
}

typedef struct CompactItem
{
	OffsetNumber off;
	uint16		lp_off;
	uint16		len;
	uint16		need;
} CompactItem;

static int
compact_item_cmp(const void *a, const void *b)
{
	const CompactItem *ia = (const CompactItem *) a;
	const CompactItem *ib = (const CompactItem *) b;

	/* descending physical position; ties by offset number */
	if (ia->lp_off != ib->lp_off)
		return ia->lp_off > ib->lp_off ? -1 : 1;
	return ia->off < ib->off ? -1 : (ia->off > ib->off ? 1 : 0);
}

/*
 * Rewrite the page's tuple area compactly, keeping "reserve[off]" bytes of
 * storage for items that need it.  Deterministic, so redo produces the
 * same layout.  "rel" is unused (kept for symmetry); may be NULL in redo.
 */
void
iheap_page_compact(Relation rel, Page page, const uint32 *reserve)
{
	PageHeader	ph = (PageHeader) page;
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	CompactItem items[MaxOffsetNumber];
	PGAlignedBlock scratch;
	int			nitems = 0;
	int			upper;
	bool		anyfree = false;

	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		lp = PageGetItemId(page, off);

		if (!ItemIdIsUsed(lp))
		{
			anyfree = true;
			continue;
		}
		if (!ItemIdHasStorage(lp))
			continue;
		items[nitems].off = off;
		items[nitems].lp_off = ItemIdGetOffset(lp);
		items[nitems].len = ItemIdGetLength(lp);
		items[nitems].need = Max(ItemIdGetLength(lp),
								 reserve ? reserve[off] : 0);
		nitems++;
	}

	qsort(items, nitems, sizeof(CompactItem), compact_item_cmp);

	memcpy(scratch.data, page, BLCKSZ);
	upper = ph->pd_special;
	for (int i = 0; i < nitems; i++)
	{
		const char *src = scratch.data + items[i].lp_off;
		int			start = ih_place_start(upper - (items[i].need - items[i].len),
										   src, items[i].len);

		if (start < ph->pd_lower)
			elog(PANIC, "iheap: page compaction overflow");
		memcpy((char *) page + start, src, items[i].len);
		if (items[i].need > items[i].len)
			memset((char *) page + start + items[i].len, 0,
				   items[i].need - items[i].len);
		ItemIdSetNormal(PageGetItemId(page, items[i].off), start, items[i].len);
		upper = start;
	}

	/* zero the gap so that pages are byte-identical on standbys */
	memset((char *) page + ph->pd_lower, 0, upper - ph->pd_lower);
	ph->pd_upper = upper;

	if (anyfree)
		PageSetHasFreeLinePointers(page);
	else
		PageClearHasFreeLinePointers(page);
}

/*
 * Do the tuples of a page fit after compaction?  Items that become LP_DEAD
 * (gone) lose their storage, stubs keep a header, every item takes the
 * larger of its length and its reservation: the placement of
 * iheap_page_compact(), done on paper.
 */
static bool
compaction_fits(Page page, const bool *gone, const bool *stubbed,
				const uint32 *reserve)
{
	PageHeader	ph = (PageHeader) page;
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	CompactItem items[MaxOffsetNumber];
	int			nitems = 0;
	int			upper = ph->pd_special;

	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		lp = PageGetItemId(page, off);

		if (!ItemIdHasStorage(lp) || gone[off])
			continue;
		items[nitems].off = off;
		items[nitems].lp_off = ItemIdGetOffset(lp);
		items[nitems].len = stubbed[off] ? IHEAP_TUPHDR_SIZE : ItemIdGetLength(lp);
		items[nitems].need = Max(items[nitems].len, reserve[off]);
		nitems++;
	}
	qsort(items, nitems, sizeof(CompactItem), compact_item_cmp);
	for (int i = 0; i < nitems; i++)
	{
		int			start = ih_place_start(upper - (items[i].need - items[i].len),
										   (char *) page + items[i].lp_off,
										   items[i].len);

		if (start < ph->pd_lower)
			return false;
		upper = start;
	}
	return true;
}

/*
 * Has the writer that the page's room hint names ended (§21 D45)?  Then
 * the storage it kept reserved for its stubs can be given back.  A value
 * that is not a recent xid, left long ago, counts as ended.
 */
static bool
room_writer_ended(Page page)
{
	TransactionId xid = IHeapPageGetOpaque(page)->ihp_room_xid;

	if (!TransactionIdIsNormal(xid))
		return false;
	return !TransactionIdPrecedes(xid, ReadNextTransactionId()) ||
		!TransactionIdIsInProgress(xid);
}

/*
 * Prune a data page held with an exclusive lock: roll back aborted changes,
 * shrink deletions every snapshot sees to stubs, turn frozen deletions into
 * LP_DEAD, and compact.  With grow_off, the item there gets storage for
 * grow_len bytes where it is, if the page has the room (*grown).  Returns
 * true if anything changed.  Offsets of items that became LP_DEAD are
 * appended to deadoffs (if not NULL).
 */
static bool
prune_page(Relation rel, Buffer buffer, bool force, OffsetNumber *deadoffs,
		   int *ndead_out, OffsetNumber grow_off, uint32 grow_len, bool *grown)
{
	Page		page = BufferGetPage(buffer);
	OffsetNumber maxoff;
	OffsetNumber dead[MaxOffsetNumber];
	OffsetNumber stub[MaxOffsetNumber];
	uint32		reserve[MaxOffsetNumber + 1];
	bool		gone[MaxOffsetNumber + 1];
	bool		stubbed[MaxOffsetNumber + 1];
	int			ndead = 0;
	int			nstub = 0;
	int			nreserve = 0;
	bool		pending = false;
	bool		stubs_wait = false;
	UndoRecPtr	discard;
	GlobalVisState *vistest;
	TransactionId conflict = InvalidTransactionId;
	TransactionId wait_xid = InvalidTransactionId;
	TransactionId room_xid = InvalidTransactionId;

	if (ndead_out)
		*ndead_out = 0;
	if (grown)
		*grown = false;

	if (!IHeapPageIsData(page))
		return false;

	/* first, apply undo of aborted transactions */
	(void) iheap_rollback_page(rel, buffer);
	iheap_stat_rollback_report(RelationGetRelid(rel), IHEAP_ROLLBACK_LAZY);

	/*
	 * Stubs whose writer has ended give back the storage kept for them:
	 * compact, even if nothing else changes (§21 D45).
	 */
	if (room_writer_ended(page))
		force = true;

	discard = iheap_discard_ptr(rel);
	vistest = GlobalVisTestFor(rel);
	maxoff = PageGetMaxOffsetNumber(page);
	memset(reserve, 0, sizeof(uint32) * (maxoff + 1));
	memset(gone, 0, sizeof(bool) * (maxoff + 1));
	memset(stubbed, 0, sizeof(bool) * (maxoff + 1));

	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		lp = PageGetItemId(page, off);
		char	   *tup;
		IHeapHdr	h;

		if (!ItemIdIsNormal(lp))
			continue;
		tup = (char *) page + ItemIdGetOffset(lp);
		ih_hdr_read(tup, &h);

		if (h.infomask & (IHT_DELETED | IHT_MOVED))
		{
			IHeapXidStatus st;

			if (h.urec < discard)
			{
				dead[ndead++] = off;
				gone[off] = true;
				continue;
			}
			st = iheap_xid_status(h.xid, buffer, tup);
			if (st == IHX_COMMITTED &&
				GlobalVisTestIsRemovableXid(vistest, h.xid, true))
			{
				/* no snapshot sees the row before its deletion: a stub */
				if ((h.infomask & IHT_STUB) == 0)
				{
					stub[nstub++] = off;
					stubbed[off] = true;
					if (!TransactionIdIsValid(conflict) ||
						TransactionIdFollows(h.xid, conflict))
						conflict = h.xid;
				}
				stubs_wait = true;
			}
			else if (h.infomask & IHT_STUB)
			{
				/*
				 * The body in undo (a compact row's, or one that moved off a
				 * page whose bodies a snapshot holds, §21 D45): its room
				 * stays reserved until the writer ends, which the room hint
				 * then names.
				 */
				reserve[off] = iheap_item_reservation(rel, page, off);
				if (reserve[off] > 0)
				{
					nreserve++;
					if (!TransactionIdIsValid(room_xid) ||
						TransactionIdFollows(h.xid, room_xid))
						room_xid = h.xid;
				}
				stubs_wait = true;
			}
			else if (!TransactionIdIsValid(wait_xid) ||
					 TransactionIdPrecedes(h.xid, wait_xid))
			{
				/* the body stays until the horizon passes its deleter */
				wait_xid = h.xid;
			}
			pending = true;
		}
		else if (h.urec >= discard)
		{
			reserve[off] = iheap_item_reservation(rel, page, off);
			if (reserve[off] > 0)
			{
				nreserve++;
				pending = true;
			}
		}
	}

	if (OffsetNumberIsValid(grow_off) && grow_off <= maxoff &&
		!gone[grow_off] && !stubbed[grow_off] &&
		ItemIdIsNormal(PageGetItemId(page, grow_off)))
	{
		uint32		was = reserve[grow_off];

		reserve[grow_off] = Max(was, grow_len);
		if (compaction_fits(page, gone, stubbed, reserve))
		{
			if (was == 0)
				nreserve++;
			force = true;
			if (grown)
				*grown = true;
		}
		else
			reserve[grow_off] = was;
	}

	/*
	 * What the next prune waits for (iheap_page_prune_opt()): the oldest
	 * deleter whose row's body has to stay; failing that, if stubs wait for
	 * discard to pass their records, the next xid (once every transaction
	 * there is now has gone, discard can have passed them).  A hint, as
	 * heap's pd_prune_xid: set without WAL, masked by consistency checks.
	 */
	if (!TransactionIdIsValid(wait_xid) && stubs_wait)
		wait_xid = ReadNextTransactionId();
	IHeapPageGetOpaque(page)->ihp_room_xid = room_xid;

	if (ndead == 0 && nstub == 0 && !force)
	{
		if (!pending)
			IHeapPageGetOpaque(page)->ihp_flags &= ~IHP_PRUNABLE;
		((PageHeader) page)->pd_prune_xid = wait_xid;
		return false;
	}

	START_CRIT_SECTION();

	for (int i = 0; i < ndead; i++)
		ItemIdSetDead(PageGetItemId(page, dead[i]));
	for (int i = 0; i < nstub; i++)
	{
		ItemId		lp = PageGetItemId(page, stub[i]);
		char	   *tup = (char *) page + ItemIdGetOffset(lp);

		ih_hdr_set_infomask(tup, ih_hdr_infomask(tup) | IHT_STUB);
		ItemIdSetNormal(lp, ItemIdGetOffset(lp), IHEAP_TUPHDR_SIZE);
	}
	iheap_page_compact(rel, page, reserve);
	if (!pending)
		IHeapPageGetOpaque(page)->ihp_flags &= ~IHP_PRUNABLE;
	((PageHeader) page)->pd_prune_xid = wait_xid;
	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(rel))
	{
		xl_iheap_prune xlrec;
		XLogRecPtr	recptr;
		struct
		{
			OffsetNumber off;
			uint16		len;
		}			res[MaxOffsetNumber];
		int			nres = 0;

		for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
		{
			if (reserve[off] > 0)
			{
				res[nres].off = off;
				res[nres].len = (uint16) reserve[off];
				nres++;
			}
		}
		Assert(nres == nreserve);

		xlrec.ndead = ndead;
		xlrec.nstub = nstub;
		xlrec.nreserve = nres;
		xlrec.flags = (pending ? 0 : XLIH_PRUNE_DONE) |
			(TransactionIdIsValid(conflict) ? XLIH_PRUNE_CONFLICT : 0);

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfIHeapPrune);
		if (TransactionIdIsValid(conflict))
			XLogRegisterData(&conflict, sizeof(TransactionId));
		XLogRegisterBuffer(IH_BLK_DATA, buffer, REGBUF_STANDARD);
		if (ndead > 0)
			XLogRegisterBufData(IH_BLK_DATA, dead, ndead * sizeof(OffsetNumber));
		if (nstub > 0)
			XLogRegisterBufData(IH_BLK_DATA, stub, nstub * sizeof(OffsetNumber));
		if (nres > 0)
			XLogRegisterBufData(IH_BLK_DATA, res, nres * sizeof(res[0]));
		recptr = XLogInsert(RM_IHEAP_ID, XLOG_IHEAP_PRUNE);
		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	if (deadoffs != NULL)
		memcpy(deadoffs, dead, ndead * sizeof(OffsetNumber));
	if (ndead_out)
		*ndead_out = ndead;

	return true;
}

bool
iheap_page_prune(Relation rel, Buffer buffer, bool force,
				 OffsetNumber *deadoffs, int *ndead_out)
{
	return prune_page(rel, buffer, force, deadoffs, ndead_out,
					  InvalidOffsetNumber, 0, NULL);
}

/*
 * Does the last prune of the page wait for the horizon to pass one of its
 * deleters (pd_prune_xid)?  Until then pruning frees no body of a deleted
 * or moved row there (§21 D40).  As with heap's hint, the changes of a
 * writer that aborted wait too (writers roll back what they touch, readers
 * pass over it, VACUUM prunes regardless), and a value that is not a recent
 * xid, left long ago, is not trusted.
 */
bool
iheap_page_prune_waits(Relation rel, Page page)
{
	TransactionId hint = ((PageHeader) page)->pd_prune_xid;

	return TransactionIdIsNormal(hint) &&
		TransactionIdPrecedes(hint, ReadNextTransactionId()) &&
		!GlobalVisTestIsRemovableXid(GlobalVisTestFor(rel), hint, true);
}

/*
 * Does the prune wait for the horizon to pass a writer that has committed?
 * Then a snapshot older than that commit holds the page's bodies, and it
 * would hold the body of a row that moves off the page now as well (§21
 * D45).  Waiting only for running writers, such as the current transaction
 * moving row after row, says nothing of the snapshots.
 */
bool
iheap_page_bodies_held(Relation rel, Page page)
{
	return iheap_page_prune_waits(rel, page) &&
		TransactionIdDidCommit(((PageHeader) page)->pd_prune_xid);
}

/*
 * Prune for a writer that wants room on the page, unless the prune waits
 * for the horizon: pruning would free nothing, and while a long snapshot
 * holds the horizon every write to a full page would walk its items again
 * (§21 D40).  Stubs whose writer has ended since do not wait: compaction
 * gives back the storage kept for them (§21 D45).
 */
bool
iheap_page_prune_opt(Relation rel, Buffer buffer)
{
	Page		page = BufferGetPage(buffer);

	if (!(IHeapPageGetOpaque(page)->ihp_flags & IHP_PRUNABLE))
		return false;
	if (iheap_page_prune_waits(rel, page) && !room_writer_ended(page))
		return false;
	return prune_page(rel, buffer, false, NULL, NULL,
					  InvalidOffsetNumber, 0, NULL);
}

/*
 * A stub's storage stays reserved until xid ends (a deletion or a move
 * whose body went to undo): the room hint names the newest such writer.
 */
void
iheap_page_note_room(Page page, TransactionId xid)
{
	IHeapPageOpaque op = IHeapPageGetOpaque(page);

	if (!TransactionIdIsNormal(op->ihp_room_xid) ||
		TransactionIdFollows(xid, op->ihp_room_xid))
		op->ihp_room_xid = xid;
}

/*
 * Prune the page and give the item at "off" storage for "len" bytes where
 * it is, if the page has the room: a version that grows in an in-place
 * update and finds no free space for its new length elsewhere on the page
 * (§21 D43).  Returns whether it did.
 */
bool
iheap_page_prune_grow(Relation rel, Buffer buffer, OffsetNumber off, uint32 len)
{
	bool		grown;

	(void) prune_page(rel, buffer, true, NULL, NULL, off, len, &grown);
	return grown;
}
