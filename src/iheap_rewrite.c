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
#include "access/xloginsert.h"
#include "commands/progress.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "iheap.h"
#include "iheap_xlog.h"

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

/* write one undo record of the new relation by itself (older versions) */
static UndoRecPtr
write_undo_only(Relation rel, IHeapUndoRecHeader *uh, const char *payload,
				uint32 payload_len)
{
	IHeapUndoReservation res;
	UndoRecPtr	urp;

	memset(&res, 0, sizeof(res));
	iheap_undo_reserve(rel, IHEAP_UNDOREC_HDRSZ + payload_len, &res);

	START_CRIT_SECTION();
	urp = iheap_undo_write(rel, &res, uh, payload, payload_len);
	if (RelationNeedsWAL(rel))
	{
		XLogRecPtr	recptr;

		XLogBeginInsert();
		iheap_undo_register_wal(&res, IH_BLK_UNDO, InvalidBlockNumber);
		recptr = XLogInsert(RM_IHEAP_ID, XLOG_IHEAP_UNDO);
		iheap_undo_set_lsn(&res, recptr);
	}
	END_CRIT_SECTION();

	iheap_undo_release(&res);
	return urp;
}

/*
 * Write one row into the new relation.  chain[0] is the newest version,
 * chain[n-1] the oldest one kept.  A deletion entry takes its content from
 * the next older entry.  Undo records are written oldest first, so that
 * each can name the next older one in its prev header; the older ones go
 * in WAL records of their own, the newest one with the tuple.
 */
static void
write_row(Relation OldHeap, Relation NewHeap, Chain *chain,
		  BulkInsertStateData *bistate)
{
	ChainVersion *v = chain->v;
	int			n = chain->n;
	char	  **content = palloc(n * sizeof(char *));
	uint32	   *clen = palloc(n * sizeof(uint32));
	IHeapHdr   *hdrs = palloc(n * sizeof(IHeapHdr));
	char	  **payload = palloc(n * sizeof(char *));
	uint32	   *payload_len = palloc(n * sizeof(uint32));
	Buffer		buffer;
	Page		page;
	bool		page_init;
	IHeapUndoReservation res;
	bool		top_undo;
	OffsetNumber offnum;
	BlockNumber blkno;
	char	   *top;

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

	buffer = iheap_get_buffer_for_tuple(NewHeap, clen[0], bistate, InvalidBuffer,
										&page_init);
	page = BufferGetPage(buffer);
	blkno = BufferGetBlockNumber(buffer);
	offnum = iheap_page_choose_offnum(page);

	/*
	 * Headers of all versions, oldest first.  Records of the older versions
	 * are written now; the newest version's record goes with the tuple.
	 */
	for (int i = n - 1; i >= 0; i--)
	{
		IHeapHdr	hk;

		ih_hdr_read(content[i], &hk);
		hk.infomask &= IHT_DATA_FLAGS;

		if (!TransactionIdIsNormal(v[i].creator))
		{
			/* the version everybody sees: frozen, no undo */
			Assert(i == n - 1);
			hk.xid = FrozenTransactionId;
			hk.urec = InvalidUndoRecPtr;
		}
		else
		{
			hk.xid = v[i].creator;
			hk.urec = InvalidUndoRecPtr;	/* set below */
			if (v[i].deleted)
				hk.infomask |= IHT_DELETED | IHT_KEYS_UPDATED;
			else if (i < n - 1)
				hk.infomask |= IHT_INPLACE;

			if (i > 0)
			{
				IHeapUndoRecHeader uh;

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
					ih_hdr_write(uh.ur_prev_hdr, &hdrs[i + 1]);
				}
				hk.urec = write_undo_only(NewHeap, &uh, payload[i],
										  payload_len[i]);
			}
		}
		hdrs[i] = hk;
	}

	top_undo = TransactionIdIsNormal(v[0].creator);
	memset(&res, 0, sizeof(res));
	res.buffer = InvalidBuffer;
	if (top_undo)
		iheap_undo_reserve(NewHeap, IHEAP_UNDOREC_HDRSZ + payload_len[0], &res);

	START_CRIT_SECTION();

	if (top_undo)
	{
		IHeapUndoRecHeader uh;

		memset(&uh, 0, sizeof(uh));
		uh.ur_xid = v[0].creator;
		uh.ur_cid = FirstCommandId;
		uh.ur_blkno = blkno;
		uh.ur_offnum = offnum;
		uh.ur_xact_prev = InvalidUndoRecPtr;
		if (n == 1)
			uh.ur_type = IHU_INSERT;
		else
		{
			uh.ur_type = v[0].deleted ? IHU_DELETE : IHU_UPDATE;
			ih_hdr_write(uh.ur_prev_hdr, &hdrs[1]);
		}
		hdrs[0].urec = iheap_undo_write(NewHeap, &res, &uh, payload[0],
										payload_len[0]);
	}

	top = content[0];
	ih_hdr_write(top, &hdrs[0]);
	if (iheap_page_add_item(page, top, clen[0], offnum) != offnum)
		elog(PANIC, "iheap: rewrite failed to add tuple");
	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(NewHeap))
	{
		xl_iheap_insert xlrec;
		XLogRecPtr	recptr;

		xlrec.offnum = offnum;
		xlrec.flags = page_init ? XLIH_INIT_PAGE : 0;
		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfIHeapInsert);
		XLogRegisterBuffer(IH_BLK_DATA, buffer,
						   REGBUF_STANDARD | (page_init ? REGBUF_WILL_INIT : 0));
		XLogRegisterBufData(IH_BLK_DATA, top, clen[0]);
		if (top_undo)
			iheap_undo_register_wal(&res, IH_BLK_UNDO,
									BufferGetBlockNumber(buffer));
		recptr = XLogInsert(RM_IHEAP_ID, XLOG_IHEAP_INSERT);
		PageSetLSN(page, recptr);
		if (top_undo)
			iheap_undo_set_lsn(&res, recptr);
	}

	END_CRIT_SECTION();

	if (top_undo)
		iheap_undo_release(&res);
	UnlockReleaseBuffer(buffer);
}

/* state of one table rewrite */
typedef struct RewriteState
{
	Relation	OldTable;
	Relation	NewTable;
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
	write_row(rs->OldTable, rs->NewTable, &chain, NULL);
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
