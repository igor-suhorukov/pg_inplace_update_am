/*-------------------------------------------------------------------------
 *
 * iheap_prune.c
 *	  Data page space management: item placement, compaction, pruning.
 *
 * Space rules (DESIGN.md §21 D6):
 *	- Storage freed by a DELETE, a non-in-place UPDATE or a shrinking
 *	  in-place UPDATE stays reserved until the writer commits, because
 *	  rollback restores it in place.
 *	- After the deleter commits, a deleted row shrinks to a 16-byte stub;
 *	  old snapshots read its image from undo.  Once the stub is frozen it
 *	  becomes LP_DEAD (no storage) until index vacuum frees the line pointer.
 *	- Compaction needs only an exclusive content lock: readers never touch
 *	  page memory without holding the content lock (§21 D11).
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"

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
 * uncommitted in-place updates in its chain (0 if none).  Rollback of those
 * updates restores the old, longer tuple at the item's location.
 */
uint32
iheap_item_reservation(Relation rel, Page page, OffsetNumber off)
{
	ItemId		lp = PageGetItemId(page, off);
	IHeapHdr	h;
	UndoRecPtr	urp;
	UndoRecPtr	discard;
	uint32		reserve = 0;
	int			steps = 0;

	if (!ItemIdIsNormal(lp))
		return 0;

	ih_hdr_read((char *) page + ItemIdGetOffset(lp), &h);
	discard = iheap_discard_ptr(rel);
	if (h.urec < discard)
		return 0;
	if (iheap_xid_status(h.xid, InvalidBuffer, NULL) == IHX_COMMITTED)
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
 * Prune a data page held with an exclusive lock: roll back aborted changes,
 * shrink committed deletions to stubs, turn frozen deletions into LP_DEAD,
 * and compact.  Returns true if anything changed.  Offsets of items that
 * became LP_DEAD are appended to deadoffs (if not NULL).
 */
bool
iheap_page_prune(Relation rel, Buffer buffer, bool force,
				 OffsetNumber *deadoffs, int *ndead_out)
{
	Page		page = BufferGetPage(buffer);
	OffsetNumber maxoff;
	OffsetNumber dead[MaxOffsetNumber];
	OffsetNumber stub[MaxOffsetNumber];
	uint32		reserve[MaxOffsetNumber + 1];
	int			ndead = 0;
	int			nstub = 0;
	int			nreserve = 0;
	bool		pending = false;
	UndoRecPtr	discard;

	if (ndead_out)
		*ndead_out = 0;

	if (!IHeapPageIsData(page))
		return false;

	/* first, apply undo of aborted transactions */
	(void) iheap_rollback_page(rel, buffer);
	iheap_stat_rollback_report(RelationGetRelid(rel), IHEAP_ROLLBACK_LAZY);

	discard = iheap_discard_ptr(rel);
	maxoff = PageGetMaxOffsetNumber(page);
	memset(reserve, 0, sizeof(uint32) * (maxoff + 1));

	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		lp = PageGetItemId(page, off);
		IHeapHdr	h;

		if (!ItemIdIsNormal(lp))
			continue;
		ih_hdr_read((char *) page + ItemIdGetOffset(lp), &h);

		if (h.infomask & (IHT_DELETED | IHT_MOVED))
		{
			if (h.urec < discard)
				dead[ndead++] = off;
			else if (iheap_xid_status(h.xid, buffer,
									  (char *) page + ItemIdGetOffset(lp)) == IHX_COMMITTED)
			{
				if ((h.infomask & IHT_STUB) == 0)
					stub[nstub++] = off;
				pending = true;
			}
			else
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

	if (ndead == 0 && nstub == 0 && !force)
	{
		if (!pending)
			IHeapPageGetOpaque(page)->ihp_flags &= ~IHP_PRUNABLE;
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
		xlrec.flags = pending ? 0 : 1;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfIHeapPrune);
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
