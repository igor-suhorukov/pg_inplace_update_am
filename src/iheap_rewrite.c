/*-------------------------------------------------------------------------
 *
 * iheap_rewrite.c
 *	  MVCC-safe rewrite for VACUUM FULL, CLUSTER and REPACK.
 *
 * For each row the latest committed version is copied, together with the
 * older versions that snapshots older than OldestXmin may still need
 * (DESIGN.md §21 D17).  The older versions are written as undo records of
 * the new relation that keep their original creators' xids, so old
 * snapshots keep seeing what they saw before.  Out-of-line values of every
 * copied version are re-toasted into the new relation's TOAST table.
 *
 * Rows deleted by transactions not yet visible to everybody are kept as
 * deleted rows (with their content in undo); rows whose deletion every
 * snapshot sees are dropped.
 *
 * The new relation is built in memory a page at a time and written with
 * the bulk writer (bulk_write.h), as heap's rewrite is (§21 D41): no shared
 * buffers, no per-row WAL records, the pages WAL-logged whole.  Its undo
 * pages, in extents placed among the data pages, and its metapage, last,
 * go the same way.  A row whose only version every snapshot sees may get a
 * compact header (§21 D43).
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heaptoast.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "commands/progress.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/bulk_write.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "iheap.h"

typedef struct ChainVersion
{
	char	   *tuple;			/* content (header describes it) */
	uint32		len;
	TransactionId creator;
	bool		deleted;		/* this "version" is a deletion */
} ChainVersion;

/* versions of one row, v[0] the newest; allocated in the per-row context */
typedef struct Chain
{
	ChainVersion *v;
	int			n;
	int			cap;
} Chain;

static void
chain_add(Chain *c, char *tuple, uint32 len, TransactionId creator,
		  bool deleted)
{
	if (c->n >= c->cap)
	{
		c->cap = c->cap > 0 ? c->cap * 2 : 8;
		if (c->v == NULL)
			c->v = palloc(c->cap * sizeof(ChainVersion));
		else
			c->v = repalloc(c->v, c->cap * sizeof(ChainVersion));
	}
	c->v[c->n].tuple = tuple;
	c->v[c->n].len = len;
	c->v[c->n].creator = creator;
	c->v[c->n].deleted = deleted;
	c->n++;
}

/*
 * Collect the versions of an item that must survive: from the newest back
 * to the newest version visible to everybody.  Leaves c->n = 0 if the row
 * is to be dropped.
 */
static void
collect_chain(Relation rel, Page page, OffsetNumber off,
			  TransactionId OldestXmin, Chain *c, bool *recently_dead)
{
	ItemId		lp = PageGetItemId(page, off);
	char	   *pagetup = (char *) page + ItemIdGetOffset(lp);
	IHeapHdr	h;
	char	   *cur;
	uint32		curlen;
	char	   *mem;

	*recently_dead = false;
	c->n = 0;
	cur = ih_copy_tuple_aligned(CurrentMemoryContext, pagetup,
								ItemIdGetLength(lp), &mem);
	curlen = ItemIdGetLength(lp);
	ih_hdr_read(cur, &h);

	for (;;)
	{
		IHeapXidStatus st;
		IHeapUndoRec *rec;
		bool		deleted = (h.infomask & (IHT_DELETED | IHT_MOVED)) != 0;
		bool		all_visible;

		CHECK_FOR_INTERRUPTS();

		if (iheap_is_frozen(rel, &h))
		{
			/* a deletion visible to all drops the row */
			if (!deleted)
				chain_add(c, cur, curlen, FrozenTransactionId, false);
			return;
		}

		st = iheap_xid_status(h.xid, InvalidBuffer, NULL);
		if (st == IHX_IN_PROGRESS)
			elog(ERROR, "iheap: concurrent change during table rewrite");

		/* LOCK records carry no content */
		if (h.infomask & IHT_LOCKED)
		{
			rec = iheap_undo_fetch(rel, h.urec, true);
			if (rec != NULL && rec->hdr.ur_type == IHU_LOCK)
			{
				IHeapHdr	ph;

				ih_hdr_read(rec->hdr.ur_prev_hdr, &ph);
				h.urec = ph.urec;
				h.infomask = ph.infomask | (h.infomask & IHT_STUB);
				pfree(rec);
				continue;
			}
			if (rec)
				pfree(rec);
		}

		all_visible = (st == IHX_COMMITTED || st == IHX_CURRENT) &&
			TransactionIdPrecedes(h.xid, OldestXmin);

		if (st != IHX_ABORTED)
		{
			if (all_visible)
			{
				if (!deleted)
					chain_add(c, cur, curlen, FrozenTransactionId, false);
				else if (c->n > 0 && c->v[c->n - 1].deleted)
					c->n = 0;	/* cannot happen: deletions are newest */
				return;
			}
			chain_add(c, cur, curlen, h.xid, deleted);
			if (deleted && c->n == 1)
				*recently_dead = true;
		}

		/* step back */
		rec = iheap_undo_fetch(rel, h.urec, true);
		if (rec == NULL)
		{
			if (c->n > 0 && c->v[c->n - 1].deleted)
				c->n = 0;
			return;
		}
		if (!iheap_rebuild_prior(rel, &cur, &curlen, &h, rec, &mem))
		{
			/* inserted by a creator not visible to all: chain ends here */
			pfree(rec);
			if (c->n > 0 && c->v[c->n - 1].deleted)
				c->n = 0;
			return;
		}
		pfree(rec);
	}
}

/*
 * Deform with the old relation's descriptor (it has the missing values of
 * columns added without a rewrite, the new one does not), null out dropped
 * columns, re-toast into the new relation and form again, as heap's
 * reform_and_rewrite_tuple() does.
 */
static char *
retoast_version(Relation OldHeap, Relation NewHeap, const char *tuple,
				uint32 *len)
{
	TupleDesc	olddesc = RelationGetDescr(OldHeap);
	TupleDesc	desc = RelationGetDescr(NewHeap);
	Datum	   *values = palloc(Max(desc->natts, 1) * sizeof(Datum));
	bool	   *isnull = palloc(Max(desc->natts, 1) * sizeof(bool));
	char	   *mem;
	char	   *res;
	IHeapToastState *ts = NULL;

	Assert(olddesc->natts == desc->natts);
	ih_deform_tuple(olddesc, tuple, values, isnull);
	for (int i = 0; i < desc->natts; i++)
	{
		if (TupleDescCompactAttr(desc, i)->attisdropped)
			isnull[i] = true;
	}
	if (iheap_tuple_needs_toast(NewHeap, desc, values, isnull, false))
	{
		ts = palloc(sizeof(IHeapToastState));
		iheap_toast_begin(ts, NewHeap, desc, values, isnull, NULL, NULL, 0);
	}
	res = ih_form_tuple(desc, values, isnull, len, &mem);
	if (ts)
		iheap_toast_end(ts);
	if (*len > IHEAP_MAX_TUPLE_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("row is too big: size %u, maximum size %zu",
						*len, (Size) IHEAP_MAX_TUPLE_SIZE)));
	return res;
}

/*
 * The new relation as it is written: the data page and the undo page being
 * filled, in memory, and where the next pages go.
 */
typedef struct RewriteWriter
{
	Relation	rel;
	BulkWriteState *bulk;
	BlockNumber nextblk;		/* the next block to hand out */
	Size		save_free;		/* fillfactor's free space per data page */
	int			quota;			/* compact headers a page may have, -1: any */

	BulkWriteBuffer dpage;		/* data page being filled, or NULL */
	BlockNumber dblk;
	int			dcompact;		/* compact headers on it */

	BulkWriteBuffer upage;		/* undo page being filled, or NULL */
	BlockNumber ublk;
	uint64		next_lpage;		/* next logical undo page */
	IHeapExtent *extents;		/* undo extents so far */
	int			nextents;
} RewriteWriter;

static void
rw_begin(RewriteWriter *w, Relation rel)
{
	memset(w, 0, sizeof(*w));
	w->rel = rel;
	w->bulk = smgr_bulk_start_rel(rel, MAIN_FORKNUM);
	w->nextblk = IHEAP_METAPAGE_BLKNO + 1;
	w->save_free = RelationGetTargetPageFreeSpace(rel, HEAP_DEFAULT_FILLFACTOR);
	w->quota = iheap_compact_quota(rel);
	w->next_lpage = 1;
	w->extents = palloc(IHEAP_MAX_EXTENTS * sizeof(IHeapExtent));
}

static void
rw_flush_data(RewriteWriter *w)
{
	if (w->dpage == NULL)
		return;
	smgr_bulk_write(w->bulk, w->dblk, w->dpage, true);
	w->dpage = NULL;
}

static void
rw_flush_undo(RewriteWriter *w)
{
	if (w->upage == NULL)
		return;
	smgr_bulk_write(w->bulk, w->ublk, w->upage, true);
	w->upage = NULL;
}

/*
 * The data page for a row of len bytes, flushing the current one when the
 * row does not fit it with the free space fillfactor keeps; a new page takes
 * any row.  With compactable (a row whose only version is frozen), the row
 * gets a compact header if the page's quota allows: *compact.
 */
static Page
rw_data_page(RewriteWriter *w, uint32 len, bool compactable, bool *compact)
{
	uint32		shrink = IHEAP_TUPHDR_SIZE - IHEAP_COMPACT_HDRSZ;

	for (;;)
	{
		Page		page;
		bool		empty;
		Size		need;

		if (w->dpage == NULL)
		{
			w->dpage = smgr_bulk_get_buf(w->bulk);
			iheap_init_data_page((Page) w->dpage);
			w->dblk = w->nextblk++;
			w->dcompact = 0;
		}
		page = (Page) w->dpage;
		*compact = compactable && (w->quota < 0 || w->dcompact < w->quota);
		need = (*compact ? len - shrink : len) + sizeof(ItemIdData) + MAXIMUM_ALIGNOF;
		empty = PageGetMaxOffsetNumber(page) == 0;
		if (iheap_page_free_space(page) >= need + (empty ? 0 : w->save_free))
		{
			if (*compact)
				w->dcompact++;
			return page;
		}
		if (empty)
			elog(ERROR, "iheap: row of %u bytes does not fit an empty page", len);
		rw_flush_data(w);
	}
}

/* a new undo page, in the current extent or a new one after the last block */
static void
rw_new_undo_page(RewriteWriter *w)
{
	IHeapExtent *e = w->nextents > 0 ? &w->extents[w->nextents - 1] : NULL;
	uint64		lpage = w->next_lpage++;

	rw_flush_undo(w);
	if (e == NULL || lpage >= e->ie_lstart + e->ie_npages)
	{
		/* sized as the undo so far, like iheap_meta_next_extent_size() */
		uint32		npages = IHEAP_EXTENT_MIN_PAGES;

		while (npages < lpage - 1 && npages < IHEAP_EXTENT_MAX_PAGES)
			npages *= 2;
		if (w->nextents >= IHEAP_MAX_EXTENTS)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("too many undo extents in relation \"%s\"",
							RelationGetRelationName(w->rel))));
		e = &w->extents[w->nextents++];
		e->ie_lstart = lpage;
		e->ie_pstart = w->nextblk;
		e->ie_npages = npages;
		w->nextblk += npages;
	}
	w->upage = smgr_bulk_get_buf(w->bulk);
	iheap_undo_init_page((Page) w->upage, lpage);
	w->ublk = e->ie_pstart + (BlockNumber) (lpage - e->ie_lstart);
}

/* write an undo record of the new relation; returns where it went */
static UndoRecPtr
rw_undo_write(RewriteWriter *w, IHeapUndoRecHeader *uh, const char *payload,
			  uint32 payload_len)
{
	Page		page = (Page) w->upage;
	uint32		off;

	if (page == NULL ||
		((PageHeader) page)->pd_upper - ((PageHeader) page)->pd_lower <
		IHEAP_UNDOREC_HDRSZ + payload_len)
	{
		rw_new_undo_page(w);
		page = (Page) w->upage;
	}
	off = ((PageHeader) page)->pd_lower;
	(void) iheap_undo_put_record(page, off, uh, payload, payload_len);
	return MakeUndoRecPtr(IHeapUndoPageGetHeader(page)->uph_lpage, off);
}

/*
 * The last pages: the open data and undo pages, empty undo pages for the
 * rest of the last extent where it lies beyond everything else (its blocks
 * must exist), and the metapage describing the undo extents.
 */
static void
rw_finish(RewriteWriter *w)
{
	BulkWriteBuffer metabuf;
	IHeapMetaPageData *meta;
	uint64		live = 0;

	rw_flush_data(w);
	rw_flush_undo(w);
	if (w->nextents > 0)
	{
		IHeapExtent *e = &w->extents[w->nextents - 1];

		if (e->ie_pstart + e->ie_npages == w->nextblk)
		{
			for (uint64 lp = w->next_lpage; lp < e->ie_lstart + e->ie_npages; lp++)
			{
				BulkWriteBuffer b = smgr_bulk_get_buf(w->bulk);

				iheap_undo_init_page((Page) b, lp);
				smgr_bulk_write(w->bulk,
								e->ie_pstart + (BlockNumber) (lp - e->ie_lstart),
								b, true);
			}
		}
	}

	metabuf = smgr_bulk_get_buf(w->bulk);
	iheap_init_metapage((Page) metabuf);
	meta = IHeapPageGetMeta((Page) metabuf);
	meta->im_next_lpage = w->next_lpage;
	meta->im_nextents = w->nextents;
	memcpy(meta->im_extents, w->extents, w->nextents * sizeof(IHeapExtent));
	for (int i = 0; i < w->nextents; i++)
		live += w->extents[i].ie_npages;
	meta->im_undo_peak = (uint32) Min(live, PG_UINT32_MAX);
	iheap_meta_set_lower((Page) metabuf);
	smgr_bulk_write(w->bulk, IHEAP_METAPAGE_BLKNO, metabuf, true);

	smgr_bulk_finish(w->bulk);
	pfree(w->extents);
	iheap_relcache_invalidate(w->rel);
}

/*
 * Write one row into the new relation.  chain[0] is the newest version,
 * chain[n-1] the oldest one kept.  A deletion entry takes its content from
 * the next older entry.  Undo records are written oldest first, so that
 * each can name the next older one in its prev header.
 */
static void
write_row(RewriteWriter *w, Relation OldHeap, Chain *chain)
{
	Relation	NewHeap = w->rel;
	ChainVersion *v = chain->v;
	int			n = chain->n;
	char	  **content = palloc(n * sizeof(char *));
	uint32	   *clen = palloc(n * sizeof(uint32));
	IHeapHdr   *hdrs = palloc(n * sizeof(IHeapHdr));
	char	  **payload = palloc(n * sizeof(char *));
	uint32	   *payload_len = palloc(n * sizeof(uint32));
	Page		page;
	bool		compact;
	OffsetNumber offnum;
	BlockNumber blkno;

	Assert(n > 0 && !v[n - 1].deleted);

	/* re-toast every distinct content into the new relation */
	for (int i = n - 1; i >= 0; i--)
	{
		if (v[i].deleted)
		{
			content[i] = content[i + 1];
			clen[i] = clen[i + 1];
		}
		else
			content[i] = retoast_version(OldHeap, NewHeap, v[i].tuple, &clen[i]);

		/* payload of the record that leads from version i to i + 1 */
		payload[i] = NULL;
		payload_len[i] = 0;
		if (TransactionIdIsNormal(v[i].creator) && i < n - 1)
		{
			if (v[i].deleted)
			{
				/* DELETE: the body of the version before the deletion */
				payload[i] = content[i + 1] + IHEAP_TUPHDR_SIZE;
				payload_len[i] = clen[i + 1] - IHEAP_TUPHDR_SIZE;
			}
			else
			{
				/* UPDATE: the whole old body as the changed middle part */
				IHeapUpdateDelta d;
				uint32		oldbody = clen[i + 1] - IHEAP_TUPHDR_SIZE;

				d.old_len = (uint16) clen[i + 1];
				d.prefix = 0;
				d.suffix = 0;
				d.mid_len = (uint16) oldbody;
				payload_len[i] = sizeof(d) + oldbody;
				payload[i] = palloc(payload_len[i]);
				memcpy(payload[i], &d, sizeof(d));
				memcpy(payload[i] + sizeof(d), content[i + 1] + IHEAP_TUPHDR_SIZE,
					   oldbody);
			}
		}
	}

	/* a row whose only version everybody sees may be compact (§21 D43) */
	page = rw_data_page(w, clen[0],
						n == 1 && !TransactionIdIsNormal(v[0].creator) &&
						w->quota != 0 && ih_tuple_can_compact(content[0], clen[0]),
						&compact);
	if (compact)
		clen[0] = ih_tuple_compact(content[0], clen[0]);
	blkno = w->dblk;
	offnum = iheap_page_choose_offnum(page);

	/* headers of all versions and their undo records, oldest first */
	for (int i = n - 1; i >= 0; i--)
	{
		IHeapHdr	hk;

		ih_hdr_read(content[i], &hk);
		hk.infomask &= IHT_DATA_FLAGS | IHT_COMPACT;

		if (!TransactionIdIsNormal(v[i].creator))
		{
			/* the version everybody sees: frozen, no undo */
			Assert(i == n - 1);
			hk.xid = FrozenTransactionId;
			hk.urec = InvalidUndoRecPtr;
		}
		else
		{
			IHeapUndoRecHeader uh;

			hk.xid = v[i].creator;
			if (v[i].deleted)
				hk.infomask |= IHT_DELETED | IHT_KEYS_UPDATED;
			else if (i < n - 1)
				hk.infomask |= IHT_INPLACE;

			memset(&uh, 0, sizeof(uh));
			uh.ur_xid = v[i].creator;
			uh.ur_cid = FirstCommandId;
			uh.ur_blkno = blkno;
			uh.ur_offnum = offnum;
			uh.ur_xact_prev = InvalidUndoRecPtr;
			if (i == n - 1)
				uh.ur_type = IHU_INSERT;
			else
			{
				uh.ur_type = v[i].deleted ? IHU_DELETE : IHU_UPDATE;
				ih_hdr_write_image(uh.ur_prev_hdr, &hdrs[i + 1]);
			}
			hk.urec = rw_undo_write(w, &uh, payload[i], payload_len[i]);
		}
		hdrs[i] = hk;
	}

	ih_hdr_write(content[0], &hdrs[0]);
	if (iheap_page_add_item(page, content[0], clen[0], offnum) != offnum)
		elog(ERROR, "iheap: rewrite failed to add tuple");
}

/* state of one table rewrite */
typedef struct RewriteState
{
	Relation	OldTable;
	Relation	NewTable;
	RewriteWriter writer;
	TransactionId OldestXmin;
	MemoryContext rowcxt;
	TransactionId minxid;
	double		num_tuples;
	double		tups_vacuumed;
	double		tups_recently_dead;
	uint8	   *visited;		/* one bit per possible item of each block */
} RewriteState;

#define VISITED_BYTES	((IHEAP_MAX_ITEMS + 1 + 7) / 8)

static inline bool
visited_test_and_set(RewriteState *rs, BlockNumber blk, OffsetNumber off)
{
	uint8	   *b = &rs->visited[(Size) blk * VISITED_BYTES + off / 8];
	uint8		bit = 1 << (off % 8);
	bool		was = (*b & bit) != 0;

	*b |= bit;
	return was;
}

/*
 * Copy the row at "off" of the share-locked buffer to the new relation.
 * The lock is released while writing (re-toasting may do I/O; the table is
 * exclusively locked anyway) and taken again before returning.
 */
static void
rewrite_item(RewriteState *rs, Buffer buf, OffsetNumber off)
{
	Page		page = BufferGetPage(buf);
	ItemId		lp = PageGetItemId(page, off);
	Chain		chain = {0};
	bool		recently_dead;
	MemoryContext oldcxt;

	if (!ItemIdIsNormal(lp))
	{
		if (ItemIdIsDead(lp))
			rs->tups_vacuumed += 1;
		return;
	}

	oldcxt = MemoryContextSwitchTo(rs->rowcxt);
	collect_chain(rs->OldTable, page, off, rs->OldestXmin, &chain,
				  &recently_dead);
	MemoryContextSwitchTo(oldcxt);

	if (chain.n == 0)
	{
		rs->tups_vacuumed += 1;
		MemoryContextReset(rs->rowcxt);
		return;
	}

	for (int i = 0; i < chain.n; i++)
	{
		if (TransactionIdIsNormal(chain.v[i].creator) &&
			(!TransactionIdIsValid(rs->minxid) ||
			 TransactionIdPrecedes(chain.v[i].creator, rs->minxid)))
			rs->minxid = chain.v[i].creator;
	}

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	oldcxt = MemoryContextSwitchTo(rs->rowcxt);
	write_row(&rs->writer, rs->OldTable, &chain);
	MemoryContextSwitchTo(oldcxt);
	LockBuffer(buf, BUFFER_LOCK_SHARE);

	if (recently_dead)
		rs->tups_recently_dead += 1;
	else
		rs->num_tuples += 1;
	MemoryContextReset(rs->rowcxt);
}

/*
 * VACUUM FULL, CLUSTER and REPACK.  With an index (CLUSTER), rows are
 * copied in the order of that index, read with SnapshotAny so that every
 * row with a version someone may still need is reached; a physical pass
 * then copies whatever the index did not lead to.  Without one, rows are
 * copied in physical order.  (use_sort is ignored: the index order is
 * always obtained by scanning the index.)
 */
void
iheap_relation_copy_for_cluster(Relation OldTable, Relation NewTable,
								Relation OldIndex, bool use_sort,
								TransactionId OldestXmin, Snapshot snapshot,
								TransactionId *xid_cutoff,
								MultiXactId *multi_cutoff,
								double *num_tuples, double *tups_vacuumed,
								double *tups_recently_dead)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(OldTable);
	BufferAccessStrategy strategy = GetAccessStrategy(BAS_BULKREAD);
	RewriteState rs;

	if (snapshot != NULL && IsMVCCSnapshot(snapshot))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("concurrent rewrite of iheap tables is not supported")));

	memset(&rs, 0, sizeof(rs));
	rs.OldTable = OldTable;
	rs.NewTable = NewTable;
	rs.OldestXmin = OldestXmin;
	rs.minxid = InvalidTransactionId;
	rs.rowcxt = AllocSetContextCreate(CurrentMemoryContext, "iheap rewrite row",
									  ALLOCSET_DEFAULT_SIZES);
	rs.visited = MemoryContextAllocExtended(CurrentMemoryContext,
											Max((Size) nblocks * VISITED_BYTES, 1),
											MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
	rw_begin(&rs.writer, NewTable);

	pgstat_progress_update_param(PROGRESS_REPACK_TOTAL_HEAP_BLKS, nblocks);

	if (OldIndex != NULL)
	{
		IndexScanDesc iscan;
		ItemPointer tid;

		pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
									 PROGRESS_REPACK_PHASE_INDEX_SCAN_HEAP);
		iscan = index_beginscan(OldTable, OldIndex, SnapshotAny, NULL, 0, 0,
								SO_NONE);
		index_rescan(iscan, NULL, 0, NULL, 0);
		while ((tid = index_getnext_tid(iscan, ForwardScanDirection)) != NULL)
		{
			BlockNumber blk = ItemPointerGetBlockNumber(tid);
			OffsetNumber off = ItemPointerGetOffsetNumber(tid);
			Buffer		buf;
			Page		page;

			CHECK_FOR_INTERRUPTS();
			if (blk == IHEAP_METAPAGE_BLKNO || blk >= nblocks ||
				off < FirstOffsetNumber || off > IHEAP_MAX_ITEMS ||
				visited_test_and_set(&rs, blk, off))
				continue;

			buf = ReadBufferExtended(OldTable, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			if (IHeapPageIsData(page) && off <= PageGetMaxOffsetNumber(page))
				rewrite_item(&rs, buf, off);
			UnlockReleaseBuffer(buf);
		}
		index_endscan(iscan);
		pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
									 PROGRESS_REPACK_PHASE_SEQ_SCAN_HEAP);
	}

	/* physical order (the remainder, with an index) */
	for (BlockNumber blk = 1; blk < nblocks; blk++)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber maxoff;

		CHECK_FOR_INTERRUPTS();

		buf = ReadBufferExtended(OldTable, MAIN_FORKNUM, blk, RBM_NORMAL, strategy);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!IHeapPageIsData(page))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		maxoff = PageGetMaxOffsetNumber(page);
		for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
		{
			if (!visited_test_and_set(&rs, blk, off))
				rewrite_item(&rs, buf, off);
		}
		UnlockReleaseBuffer(buf);
		pgstat_progress_update_param(PROGRESS_REPACK_HEAP_BLKS_SCANNED, blk + 1);
	}

	rw_finish(&rs.writer);

	/* every normal xid stored in the new relation is at least minxid */
	if (TransactionIdIsValid(rs.minxid) && TransactionIdPrecedes(rs.minxid, *xid_cutoff))
		*xid_cutoff = rs.minxid;

	*num_tuples = rs.num_tuples;
	*tups_vacuumed = rs.tups_vacuumed;
	*tups_recently_dead = rs.tups_recently_dead;

	pfree(rs.visited);
	MemoryContextDelete(rs.rowcxt);
	FreeAccessStrategy(strategy);
}
