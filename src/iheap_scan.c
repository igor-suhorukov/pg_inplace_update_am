/*-------------------------------------------------------------------------
 *
 * iheap_scan.c
 *	  Sequential, parallel, TID-range, bitmap, sample and ANALYZE scans, and
 *	  single-tuple fetches (index scans, row refetches).
 *
 * Scans work a page at a time: while holding the share lock, the page is
 * copied into scan-private memory and the version each item shows to the
 * snapshot is resolved; the lock is then released and tuples are returned
 * from the copy (or from memory holding versions rebuilt from undo).  Two
 * copies alternate so that a slot filled from the previous page stays valid
 * while the next one is read.
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/skey.h"
#include "access/tableam.h"
#include "access/tsmapi.h"
#include "access/xact.h"
#include "executor/instrument_node.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "nodes/tidbitmap.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "storage/read_stream.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "iheap.h"

/* page copies and version memory, double-buffered */
typedef struct IHeapScanPages
{
	char	   *copy[2];
	MemoryContext cxt[2];
	int			cur;
} IHeapScanPages;

typedef struct IHeapScanExt
{
	IHeapScanDescData d;
	IHeapScanPages pages;
	int			ndead;			/* analyze: dead items on current page */
} IHeapScanExt;

static void
pages_init(IHeapScanPages *p)
{
	for (int i = 0; i < 2; i++)
	{
		p->copy[i] = palloc_aligned(BLCKSZ, PG_IO_ALIGN_SIZE, 0);
		p->cxt[i] = AllocSetContextCreate(CurrentMemoryContext,
										  "iheap scan versions",
										  ALLOCSET_SMALL_SIZES);
	}
	p->cur = 0;
}

static char *
pages_next(IHeapScanPages *p, MemoryContext *cxt)
{
	p->cur = 1 - p->cur;
	MemoryContextReset(p->cxt[p->cur]);
	*cxt = p->cxt[p->cur];
	return p->copy[p->cur];
}

static void
pages_free(IHeapScanPages *p)
{
	for (int i = 0; i < 2; i++)
	{
		if (p->copy[i])
			pfree(p->copy[i]);
		if (p->cxt[i])
			MemoryContextDelete(p->cxt[i]);
		p->copy[i] = NULL;
		p->cxt[i] = NULL;
	}
}

/* make sure a returned version's data area is aligned when it must be */
static void
ensure_aligned(IHeapVersion *v, MemoryContext cxt)
{
	if (ih_tuple_needs_align(v->tuple) &&
		((uintptr_t) (v->tuple + ih_hdr_hoff(v->tuple)) % MAXIMUM_ALIGNOF) != 0)
	{
		char	   *mem;

		v->tuple = ih_copy_tuple_aligned(cxt, v->tuple, v->len, &mem);
	}
}

/*
 * Resolve the visible versions of a page.  The caller holds a share (or
 * stronger) lock on "buffer".  "only" restricts the work to the given
 * offsets (ascending), or NULL for all items.  With predlock, a
 * serializable reader takes a tuple-level SIREAD lock on each visible row
 * (scans without a relation-level lock, as heap's bitmap scans do), while
 * the buffer is still locked so that no writer can slip in between.
 */
int
iheap_collect_page_versions(Relation rel, Buffer buffer, Snapshot snapshot,
							char *pagecopy, OffsetNumber *offs,
							IHeapVersion *vers, MemoryContext cxt,
							const OffsetNumber *only, int nonly,
							bool predlock)
{
	Page		page = BufferGetPage(buffer);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	MemoryContext oldcxt;
	int			n = 0;
	int			count = only ? nonly : maxoff;
	bool		ssi = CheckForSerializableConflictOutNeeded(rel, snapshot);

	memcpy(pagecopy, page, BLCKSZ);
	oldcxt = MemoryContextSwitchTo(cxt);

	for (int i = 0; i < count; i++)
	{
		OffsetNumber off = only ? only[i] : (OffsetNumber) (i + 1);
		ItemId		lp;
		IHeapVersion v;
		bool		visible;

		if (off < FirstOffsetNumber || off > maxoff)
			continue;
		lp = PageGetItemId(page, off);
		if (!ItemIdIsNormal(lp))
			continue;

		/* SSI: conflicts out to the versions passed over, while resolving */
		visible = iheap_resolve_version_ext(rel, buffer,
											(char *) page + ItemIdGetOffset(lp),
											ItemIdGetLength(lp),
											pagecopy + ItemIdGetOffset(lp),
											snapshot, &v, false, ssi);
		if (!visible)
			continue;
		if (predlock)
		{
			ItemPointerData tid;

			ItemPointerSet(&tid, BufferGetBlockNumber(buffer), off);
			PredicateLockTID(rel, &tid, snapshot, v.xmin);
		}
		ensure_aligned(&v, cxt);
		offs[n] = off;
		vers[n] = v;
		n++;
	}

	MemoryContextSwitchTo(oldcxt);
	return n;
}

/* ------------------------------------------------------------------------
 * Sequential and parallel scans
 * ------------------------------------------------------------------------
 */

/*
 * The block itself if it may hold data, else the nearest block in the scan
 * direction that may (InvalidBlockNumber if none below it, backward).
 */
static BlockNumber
skip_nondata(IHeapScanDesc scan, BlockNumber blk, bool forward)
{
	for (;;)
	{
		int			lo = 0;
		int			hi = scan->rs_nskip - 1;
		IHeapExtent *hit = NULL;

		while (lo <= hi)
		{
			int			mid = (lo + hi) / 2;
			IHeapExtent *e = &scan->rs_skip[mid];

			if (blk < e->ie_pstart)
				hi = mid - 1;
			else if (blk >= e->ie_pstart + e->ie_npages)
				lo = mid + 1;
			else
			{
				hit = e;
				break;
			}
		}
		if (hit == NULL)
			return blk;
		if (forward)
			blk = hit->ie_pstart + hit->ie_npages;
		else if (hit->ie_pstart == 0)
			return InvalidBlockNumber;
		else
			blk = hit->ie_pstart - 1;
	}
}

static BlockNumber
stream_next_serial(ReadStream *stream, void *private_data, void *per_buffer_data)
{
	IHeapScanDesc scan = (IHeapScanDesc) private_data;
	BlockNumber lo = scan->rs_startblock;
	BlockNumber hi = scan->rs_nblocks;

	if (scan->rs_numblocks != InvalidBlockNumber)
		hi = Min(hi, lo + scan->rs_numblocks);

	if (!scan->rs_inited)
	{
		scan->rs_inited = true;
		if (lo >= hi)
			scan->rs_prefetch_block = InvalidBlockNumber;
		else if (ScanDirectionIsBackward(scan->rs_dir))
			scan->rs_prefetch_block = hi - 1;
		else
			scan->rs_prefetch_block = lo;
	}
	else if (scan->rs_prefetch_block == InvalidBlockNumber)
		return InvalidBlockNumber;
	else if (ScanDirectionIsBackward(scan->rs_dir))
	{
		if (scan->rs_prefetch_block <= lo)
			scan->rs_prefetch_block = InvalidBlockNumber;
		else
			scan->rs_prefetch_block--;
	}
	else
	{
		scan->rs_prefetch_block++;
		if (scan->rs_prefetch_block >= hi)
			scan->rs_prefetch_block = InvalidBlockNumber;
	}

	/* undo extents and the metapage are not read */
	if (scan->rs_prefetch_block != InvalidBlockNumber)
	{
		bool		forward = !ScanDirectionIsBackward(scan->rs_dir);
		BlockNumber b = skip_nondata(scan, scan->rs_prefetch_block, forward);

		if (b == InvalidBlockNumber || (forward ? b >= hi : b < lo))
			b = InvalidBlockNumber;
		scan->rs_prefetch_block = b;
	}

	return scan->rs_prefetch_block;
}

static BlockNumber
stream_next_parallel(ReadStream *stream, void *private_data, void *per_buffer_data)
{
	IHeapScanDesc scan = (IHeapScanDesc) private_data;
	ParallelBlockTableScanDesc pbscan =
		(ParallelBlockTableScanDesc) scan->rs_base.rs_parallel;

	if (!scan->rs_inited)
	{
		table_block_parallelscan_startblock_init(scan->rs_base.rs_rd,
												 scan->rs_parallelworkerdata,
												 pbscan,
												 scan->rs_startblock,
												 scan->rs_numblocks);
		scan->rs_inited = true;
	}
	do
	{
		scan->rs_prefetch_block =
			table_block_parallelscan_nextpage(scan->rs_base.rs_rd,
											  scan->rs_parallelworkerdata, pbscan);
	} while (scan->rs_prefetch_block != InvalidBlockNumber &&
			 skip_nondata(scan, scan->rs_prefetch_block, true) !=
			 scan->rs_prefetch_block);
	return scan->rs_prefetch_block;
}

static BlockNumber
stream_next_bitmap(ReadStream *stream, void *private_data, void *per_buffer_data)
{
	TBMIterateResult *tbmres = per_buffer_data;
	IHeapScanDesc scan = (IHeapScanDesc) private_data;

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (!tbm_iterate(&scan->rs_base.st.rs_tbmiterator, tbmres))
			return InvalidBlockNumber;
		if (tbmres->blockno >= scan->rs_nblocks)
			continue;
		return tbmres->blockno;
	}
}

static void
iheap_initscan(IHeapScanDesc scan, ScanKeyData *key, bool keep_startblock)
{
	Relation	rel = scan->rs_base.rs_rd;

	if (scan->rs_base.rs_parallel != NULL)
		scan->rs_nblocks = ((ParallelBlockTableScanDesc) scan->rs_base.rs_parallel)->phs_nblocks;
	else
		scan->rs_nblocks = RelationGetNumberOfBlocks(rel);

	/* taken after nblocks: covers every extent among those blocks */
	if (scan->rs_skip)
		pfree(scan->rs_skip);
	scan->rs_nskip = iheap_nondata_ranges(rel, &scan->rs_skip);

	if ((scan->rs_base.rs_flags & SO_ALLOW_STRAT) &&
		!RelationUsesLocalBuffers(rel) && scan->rs_nblocks > (BlockNumber) (NBuffers / 4))
	{
		if (scan->rs_strategy == NULL)
			scan->rs_strategy = GetAccessStrategy(BAS_BULKREAD);
	}
	else if (scan->rs_strategy != NULL)
	{
		FreeAccessStrategy(scan->rs_strategy);
		scan->rs_strategy = NULL;
	}

	if (!keep_startblock)
		scan->rs_startblock = 0;
	scan->rs_numblocks = InvalidBlockNumber;
	scan->rs_inited = false;
	scan->rs_cblock = InvalidBlockNumber;
	scan->rs_prefetch_block = InvalidBlockNumber;
	scan->rs_ntuples = 0;
	scan->rs_cindex = 0;

	if (key != NULL && scan->rs_base.rs_nkeys > 0)
		memcpy(scan->rs_base.rs_key, key, scan->rs_base.rs_nkeys * sizeof(ScanKeyData));

	if (scan->rs_base.rs_flags & SO_TYPE_SEQSCAN)
		pgstat_count_heap_scan(rel);
}

TableScanDesc
iheap_beginscan(Relation rel, Snapshot snapshot, int nkeys, ScanKeyData *key,
				ParallelTableScanDesc pscan, uint32 flags)
{
	IHeapScanExt *ext = palloc0(sizeof(IHeapScanExt));
	IHeapScanDesc scan = &ext->d;

	/* a reader may resolve versions written before our metapage cache */
	(void) iheap_relcache(rel, false);

	scan->rs_base.rs_rd = rel;
	scan->rs_base.rs_snapshot = snapshot;
	scan->rs_base.rs_nkeys = nkeys;
	scan->rs_base.rs_flags = flags;
	scan->rs_base.rs_parallel = pscan;
	scan->rs_strategy = NULL;
	scan->rs_dir = ForwardScanDirection;
	scan->rs_analyze_buf = InvalidBuffer;

	if (pscan != NULL)
		scan->rs_parallelworkerdata = palloc(sizeof(ParallelBlockTableScanWorkerData));

	if (!(snapshot && IsMVCCSnapshot(snapshot)))
		scan->rs_base.rs_flags &= ~SO_ALLOW_PAGEMODE;

	if (scan->rs_base.rs_flags & (SO_TYPE_SEQSCAN | SO_TYPE_SAMPLESCAN))
	{
		Assert(snapshot);
		PredicateLockRelation(rel, snapshot);
	}

	if (nkeys > 0)
		scan->rs_base.rs_key = palloc(sizeof(ScanKeyData) * nkeys);

	iheap_initscan(scan, key, false);
	pages_init(&ext->pages);

	if (flags & (SO_TYPE_SEQSCAN | SO_TYPE_TIDRANGESCAN))
		scan->rs_read_stream =
			read_stream_begin_relation(READ_STREAM_SEQUENTIAL | READ_STREAM_USE_BATCHING,
									   scan->rs_strategy, rel, MAIN_FORKNUM,
									   pscan ? stream_next_parallel : stream_next_serial,
									   scan, 0);
	else if (flags & SO_TYPE_BITMAPSCAN)
		scan->rs_read_stream =
			read_stream_begin_relation(READ_STREAM_DEFAULT | READ_STREAM_USE_BATCHING,
									   scan->rs_strategy, rel, MAIN_FORKNUM,
									   stream_next_bitmap, scan,
									   sizeof(TBMIterateResult));

	if ((flags & SO_SCAN_INSTRUMENT) && scan->rs_read_stream != NULL)
	{
		scan->rs_base.rs_instrument = palloc0_object(TableScanInstrumentation);
		read_stream_enable_stats(scan->rs_read_stream,
								 &scan->rs_base.rs_instrument->io);
	}

	return (TableScanDesc) scan;
}

void
iheap_rescan(TableScanDesc sscan, ScanKeyData *key, bool set_params,
			 bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	IHeapScanDesc scan = (IHeapScanDesc) sscan;

	if (set_params)
	{
		if (allow_strat)
			scan->rs_base.rs_flags |= SO_ALLOW_STRAT;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_STRAT;
		if (allow_sync)
			scan->rs_base.rs_flags |= SO_ALLOW_SYNC;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;
		if (allow_pagemode && scan->rs_base.rs_snapshot &&
			IsMVCCSnapshot(scan->rs_base.rs_snapshot))
			scan->rs_base.rs_flags |= SO_ALLOW_PAGEMODE;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_PAGEMODE;
	}

	if (scan->rs_read_stream)
		read_stream_reset(scan->rs_read_stream);
	iheap_initscan(scan, key, true);
	scan->rs_dir = ForwardScanDirection;
}

void
iheap_endscan(TableScanDesc sscan)
{
	IHeapScanExt *ext = (IHeapScanExt *) sscan;
	IHeapScanDesc scan = &ext->d;

	if (BufferIsValid(scan->rs_analyze_buf))
		ReleaseBuffer(scan->rs_analyze_buf);
	if (scan->rs_read_stream)
		read_stream_end(scan->rs_read_stream);
	if (scan->rs_base.rs_key)
		pfree(scan->rs_base.rs_key);
	if (scan->rs_strategy != NULL)
		FreeAccessStrategy(scan->rs_strategy);
	if (scan->rs_parallelworkerdata != NULL)
		pfree(scan->rs_parallelworkerdata);
	if (scan->rs_base.rs_flags & SO_TEMP_SNAPSHOT)
		UnregisterSnapshot(scan->rs_base.rs_snapshot);
	pages_free(&ext->pages);
	if (scan->rs_skip)
		pfree(scan->rs_skip);
	pfree(ext);
}

static bool
key_test(TupleTableSlot *slot, int nkeys, ScanKeyData *keys)
{
	for (int i = 0; i < nkeys; i++)
	{
		ScanKey		k = &keys[i];
		Datum		val;
		bool		isnull;

		if (k->sk_flags & SK_ISNULL)
			return false;
		val = slot_getattr(slot, k->sk_attno, &isnull);
		if (isnull)
			return false;
		if (!DatumGetBool(FunctionCall2Coll(&k->sk_func, k->sk_collation,
											val, k->sk_argument)))
			return false;
	}
	return true;
}

/* read the next data page of the scan; false at the end */
static bool
scan_next_page(IHeapScanExt *ext, const OffsetNumber *only, int nonly)
{
	IHeapScanDesc scan = &ext->d;
	Relation	rel = scan->rs_base.rs_rd;

	for (;;)
	{
		Buffer		buf;
		Page		page;
		MemoryContext cxt;
		char	   *copy;

		CHECK_FOR_INTERRUPTS();
		buf = read_stream_next_buffer(scan->rs_read_stream, NULL);
		if (!BufferIsValid(buf))
			return false;

		scan->rs_cblock = BufferGetBlockNumber(buf);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!IHeapPageIsData(page))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}
		copy = pages_next(&ext->pages, &cxt);
		scan->rs_page = copy;
		scan->rs_ntuples = iheap_collect_page_versions(rel, buf,
													   scan->rs_base.rs_snapshot,
													   copy, scan->rs_visoff,
													   scan->rs_vis, cxt,
													   only, nonly, false);
		UnlockReleaseBuffer(buf);
		scan->rs_cindex = ScanDirectionIsBackward(scan->rs_dir) ?
			scan->rs_ntuples - 1 : 0;
		return true;
	}
}

static bool
return_current(IHeapScanDesc scan, TupleTableSlot *slot)
{
	IHeapVersion *v = &scan->rs_vis[scan->rs_cindex];

	iheap_store_tuple(slot, v->tuple, v->len, false, v->xmin, v->xmax, v->cmin);
	ItemPointerSet(&slot->tts_tid, scan->rs_cblock, scan->rs_visoff[scan->rs_cindex]);
	slot->tts_tableOid = RelationGetRelid(scan->rs_base.rs_rd);
	return true;
}

bool
iheap_getnextslot(TableScanDesc sscan, ScanDirection direction,
				  TupleTableSlot *slot)
{
	IHeapScanExt *ext = (IHeapScanExt *) sscan;
	IHeapScanDesc scan = &ext->d;

	if (ScanDirectionIsNoMovement(direction))
		direction = ForwardScanDirection;

	if (direction != scan->rs_dir)
	{
		/*
		 * Direction change: restart the stream next to the current block
		 * (whose tuples we still have), and step back over the tuple just
		 * returned.
		 */
		BlockNumber cur = scan->rs_cblock;

		scan->rs_dir = direction;
		if (scan->rs_read_stream && scan->rs_base.rs_parallel == NULL)
		{
			read_stream_reset(scan->rs_read_stream);
			scan->rs_inited = (cur != InvalidBlockNumber);
			scan->rs_prefetch_block = cur;
		}
		if (scan->rs_ntuples > 0)
			scan->rs_cindex += ScanDirectionIsBackward(direction) ? -2 : 2;
	}

	for (;;)
	{
		if (ScanDirectionIsForward(scan->rs_dir))
		{
			while (scan->rs_cindex < scan->rs_ntuples)
			{
				return_current(scan, slot);
				scan->rs_cindex++;
				if (scan->rs_base.rs_nkeys == 0 ||
					key_test(slot, scan->rs_base.rs_nkeys, scan->rs_base.rs_key))
				{
					pgstat_count_heap_getnext(scan->rs_base.rs_rd);
					return true;
				}
			}
		}
		else
		{
			while (scan->rs_cindex >= 0 && scan->rs_cindex < scan->rs_ntuples)
			{
				return_current(scan, slot);
				scan->rs_cindex--;
				if (scan->rs_base.rs_nkeys == 0 ||
					key_test(slot, scan->rs_base.rs_nkeys, scan->rs_base.rs_key))
				{
					pgstat_count_heap_getnext(scan->rs_base.rs_rd);
					return true;
				}
			}
		}

		if (!scan_next_page(ext, NULL, 0))
		{
			ExecClearTuple(slot);
			scan->rs_ntuples = 0;
			scan->rs_cindex = 0;
			/* a later change of direction restarts from the far end */
			scan->rs_cblock = InvalidBlockNumber;
			return false;
		}
	}
}

/* ------------------------------------------------------------------------
 * TID range scans
 * ------------------------------------------------------------------------
 */
void
iheap_set_tidrange(TableScanDesc sscan, ItemPointer mintid, ItemPointer maxtid)
{
	IHeapScanDesc scan = (IHeapScanDesc) sscan;
	ItemPointerData highest;
	ItemPointerData lowest;

	if (scan->rs_read_stream)
		read_stream_reset(scan->rs_read_stream);
	scan->rs_inited = false;
	scan->rs_ntuples = 0;
	scan->rs_cindex = 0;
	scan->rs_cblock = InvalidBlockNumber;

	/*
	 * Clamp the range to the relation, as heap_set_tidrange() does: an
	 * unbounded end comes in as InvalidBlockNumber, which must not be used
	 * in block arithmetic.
	 */
	ItemPointerSet(&highest, scan->rs_nblocks > 0 ? scan->rs_nblocks - 1 : 0,
				   MaxOffsetNumber);
	ItemPointerSet(&lowest, 0, FirstOffsetNumber);
	if (ItemPointerCompare(maxtid, &highest) < 0)
		ItemPointerCopy(maxtid, &highest);
	if (ItemPointerCompare(mintid, &lowest) > 0)
		ItemPointerCopy(mintid, &lowest);

	if (scan->rs_nblocks == 0 || ItemPointerCompare(&highest, &lowest) < 0)
	{
		/* empty range */
		scan->rs_startblock = 0;
		scan->rs_numblocks = 0;
	}
	else
	{
		scan->rs_startblock = ItemPointerGetBlockNumberNoCheck(&lowest);
		scan->rs_numblocks = ItemPointerGetBlockNumberNoCheck(&highest) -
			scan->rs_startblock + 1;
	}
	scan->rs_mintid = lowest;
	scan->rs_maxtid = highest;
}

bool
iheap_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction,
						   TupleTableSlot *slot)
{
	IHeapScanDesc scan = (IHeapScanDesc) sscan;

	for (;;)
	{
		if (!iheap_getnextslot(sscan, direction, slot))
			return false;
		if (ItemPointerCompare(&slot->tts_tid, &scan->rs_mintid) < 0)
		{
			if (ScanDirectionIsBackward(direction))
			{
				ExecClearTuple(slot);
				return false;
			}
			continue;
		}
		if (ItemPointerCompare(&slot->tts_tid, &scan->rs_maxtid) > 0)
		{
			if (ScanDirectionIsForward(direction))
			{
				ExecClearTuple(slot);
				return false;
			}
			continue;
		}
		return true;
	}
}

/* ------------------------------------------------------------------------
 * Single-tuple fetches
 * ------------------------------------------------------------------------
 */

/*
 * Item dead to everybody: LP_DEAD, or deleted/moved by a transaction that
 * every snapshot sees as committed (its undo need not be discarded yet).
 * Index scans then kill the entry that led here, as with heap.
 */
static bool
item_dead_to_all(Relation rel, Buffer buf, Page page, OffsetNumber off)
{
	ItemId		lp = PageGetItemId(page, off);
	char	   *tup;
	IHeapHdr	h;

	if (ItemIdIsDead(lp) || !ItemIdIsUsed(lp))
		return true;
	if (!ItemIdIsNormal(lp))
		return false;
	tup = (char *) page + ItemIdGetOffset(lp);
	ih_hdr_read(tup, &h);
	if ((h.infomask & (IHT_DELETED | IHT_MOVED)) == 0)
		return false;
	if (iheap_is_frozen(rel, &h))
		return true;
	return iheap_xid_status(h.xid, buf, tup) == IHX_COMMITTED &&
		GlobalVisTestIsRemovableXid(GlobalVisTestFor(rel), h.xid, true);
}

/*
 * TIDs that SnapshotDirty index fetches found lately (a uniqueness or
 * ON CONFLICT probe), with the version found.  The executor goes on to
 * fetch the conflicting row by TID with SnapshotAny and means the version
 * the probe saw: the newest.  ON CONFLICT DO UPDATE then locks the row,
 * which must start over if its key changed since the probe.
 */
typedef struct DirtyFound
{
	Oid			relid;
	ItemPointerData tid;
	CommandId	cid;
	UndoRecPtr	version;		/* iheap_version_urec() of the version */
} DirtyFound;

#define N_DIRTY_FOUND 4

static DirtyFound dirty_found[N_DIRTY_FOUND];
static int	dirty_found_next;

static void
note_dirty_found(Relation rel, ItemPointer tid, UndoRecPtr version)
{
	DirtyFound *d = &dirty_found[dirty_found_next];

	dirty_found_next = (dirty_found_next + 1) % N_DIRTY_FOUND;
	d->relid = RelationGetRelid(rel);
	d->tid = *tid;
	d->cid = GetCurrentCommandId(false);
	d->version = version;
}

/* as iheap_tid_found_dirty(), and which version the latest probe found */
bool
iheap_tid_found_dirty_version(Relation rel, ItemPointer tid,
							  UndoRecPtr *version)
{
	CommandId	cid = GetCurrentCommandId(false);

	for (int n = 1; n <= N_DIRTY_FOUND; n++)
	{
		DirtyFound *d = &dirty_found[(dirty_found_next + N_DIRTY_FOUND - n) %
									 N_DIRTY_FOUND];

		if (d->relid == RelationGetRelid(rel) &&
			ItemPointerEquals(&d->tid, tid) && d->cid == cid)
		{
			*version = d->version;
			return true;
		}
	}
	return false;
}

bool
iheap_tid_found_dirty(Relation rel, ItemPointer tid)
{
	CommandId	cid = GetCurrentCommandId(false);

	for (int i = 0; i < N_DIRTY_FOUND; i++)
	{
		if (dirty_found[i].relid == RelationGetRelid(rel) &&
			ItemPointerEquals(&dirty_found[i].tid, tid) &&
			dirty_found[i].cid == cid)
			return true;
	}
	return false;
}

/*
 * Does version *v of the row at off have the key of entry dm?  For
 * fetch_in_buffer(), which holds buf share-locked and saw the row's header
 * "seen" on the page.  A key value stored out of line is fetched, and an
 * index expression or predicate evaluated, with the page unlocked (TOAST
 * access takes locks and may read; expressions run the user's functions),
 * from a copy of the version that *v then holds; *changed tells that the
 * row changed meanwhile, and the caller must start over.
 */
static int
dm_matches_on_page(Relation rel, Buffer buf, OffsetNumber off,
				   const IHeapHdr *seen, const IHeapDmEntry *dm,
				   IHeapVersion *v, bool *changed)
{
	Page		page;
	ItemId		lp;
	IHeapHdr	h;
	int			match = iheap_dm_matches(rel, dm, v->tuple, 0);

	*changed = false;
	if (match >= 0)
		return match;

	if (v->mem == NULL)
		v->tuple = ih_copy_tuple_aligned(CurrentMemoryContext, v->tuple,
										 v->len, &v->mem);
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	match = iheap_dm_matches(rel, dm, v->tuple, IHEAP_DM_UNLOCKED);
	LockBuffer(buf, BUFFER_LOCK_SHARE);

	page = BufferGetPage(buf);
	if (off > PageGetMaxOffsetNumber(page) ||
		!ItemIdIsNormal(lp = PageGetItemId(page, off)))
		*changed = true;
	else
	{
		ih_hdr_read((char *) page + ItemIdGetOffset(lp), &h);
		*changed = h.xid != seen->xid || h.urec != seen->urec ||
			((h.infomask ^ seen->infomask) & ~IHT_XID_HINTS) != 0;
	}
	return match;
}

/*
 * Fetch the version of tid visible to snapshot into slot; buffer is
 * pinned by the caller.  settled_only: see iheap_resolve_version_ext().
 * dm: the dmbtree entry that led here, or NULL.  found_version, if not
 * NULL, is set to the identity (iheap_version_urec) of the version found.
 */
static bool
fetch_in_buffer(Relation rel, Buffer buf, ItemPointer tid, Snapshot snapshot,
				TupleTableSlot *slot, bool *all_dead, bool settled_only,
				const IHeapDmEntry *dm, UndoRecPtr *found_version)
{
	Page		page;
	OffsetNumber off = ItemPointerGetOffsetNumber(tid);
	ItemId		lp;
	IHeapVersion v;
	bool		visible = false;
	bool		ssi = snapshot && IsMVCCSnapshot(snapshot) &&
		CheckForSerializableConflictOutNeeded(rel, snapshot);

retry:
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	if (!IHeapPageIsData(page) || off < FirstOffsetNumber ||
		off > PageGetMaxOffsetNumber(page))
	{
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		return false;
	}

	lp = PageGetItemId(page, off);
	if (ItemIdIsNormal(lp))
	{
		char	   *tup = (char *) page + ItemIdGetOffset(lp);
		IHeapHdr	seen;
		TransactionId newest;

		ih_hdr_read(tup, &seen);
		visible = iheap_resolve_version_ext(rel, buf, tup, ItemIdGetLength(lp),
											NULL, snapshot, &v, settled_only,
											ssi);
		/* the newest change: the deletion of a deleted version */
		newest = TransactionIdIsValid(v.xmax) ? v.xmax : v.xmin;
		if (visible && settled_only && !IsolationUsesXactSnapshot() &&
			ActiveSnapshotSet() && TransactionIdIsNormal(newest) &&
			!TransactionIdIsCurrentTransactionId(newest) &&
			XidInMVCCSnapshot(newest, GetActiveSnapshot()) &&
			!iheap_tid_found_dirty(rel, tid))
		{
			IHeapHdr	h;
			IHeapVersion sv;

			/*
			 * In heap a TID names one version: the old row of an UPDATE is
			 * the version its scan saw, and so is a row an EvalPlanQual
			 * recheck (READ COMMITTED only) fetches without locking it.  An
			 * in-place update keeps the TID, so a version committed by
			 * someone else after the active snapshot gives way to the one
			 * the snapshot sees, unless a modifying lock handed it to us
			 * (iheap_version_granted: we are updating it) or a conflict
			 * probe just found it (ON CONFLICT looks at the newest).
			 */
			ih_hdr_read(tup, &h);
			if (!iheap_version_granted(rel, tid, iheap_version_urec(rel, &h),
									   GetCurrentTransactionIdIfAny(),
									   InvalidCommandId) &&
				iheap_resolve_version_ext(rel, buf, tup, ItemIdGetLength(lp),
										  NULL, GetActiveSnapshot(), &sv,
										  false, false))
			{
				if (v.mem)
					pfree(v.mem);
				v = sv;
			}
		}

		/*
		 * dmbtree: a delete-marked entry may not match the row, and an
		 * unmarked one matches its newest committed version only, so the
		 * version found is checked against the entry's key when the entry
		 * is marked or the version came from undo.  The scan copied the entry
		 * from its page earlier: an MVCC snapshot that sees a change of the
		 * key sees its mark too, but a SnapshotDirty probe sees changes made
		 * since, so other snapshots always check.
		 */
		if (visible && dm != NULL &&
			(dm->marked || v.from_undo ||
			 (!IsMVCCSnapshot(snapshot) && !dm->locked)))
		{
			bool		changed;

			if (dm_matches_on_page(rel, buf, off, &seen, dm, &v,
								   &changed) == 0 && !changed)
			{
				TransactionId running = InvalidTransactionId;

				if (snapshot->snapshot_type == SNAPSHOT_DIRTY)
					running = TransactionIdIsValid(snapshot->xmin) ?
						snapshot->xmin : snapshot->xmax;
				if (v.mem)
					pfree(v.mem);
				v.mem = NULL;
				visible = false;

				/*
				 * A uniqueness check found a running transaction's version
				 * with another key.  If the version before it has the
				 * entry's key, the row conflicts until that transaction
				 * commits: report that version as being changed by it (as
				 * heap reports the old version of a row a running
				 * transaction updates), so that the caller waits.
				 */
				if (TransactionIdIsValid(running))
				{
					IHeapVersion sv;

					/* the row is unchanged, but may have moved on the page */
					lp = PageGetItemId(page, off);
					tup = (char *) page + ItemIdGetOffset(lp);
					if (iheap_resolve_version_ext(rel, buf, tup, ItemIdGetLength(lp),
												  NULL, SnapshotAny, &sv, true,
												  false))
					{
						if (!TransactionIdIsValid(sv.xmax) &&
							dm_matches_on_page(rel, buf, off, &seen, dm, &sv,
											   &changed) == 1 && !changed)
						{
							v = sv;
							v.xmax = running;
							snapshot->xmin = InvalidTransactionId;
							snapshot->xmax = running;
							snapshot->speculativeToken = 0;
							visible = true;
						}
						else if (sv.mem)
							pfree(sv.mem);
					}
				}
			}
			if (changed)
			{
				/* a value stored out of line was fetched, and the row changed */
				if (visible && v.mem)
					pfree(v.mem);
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
				visible = false;
				goto retry;
			}
		}

		if (visible)
		{
			/* the SIREAD lock is taken before any writer can get in */
			if (snapshot && IsMVCCSnapshot(snapshot))
				PredicateLockTID(rel, tid, snapshot, v.xmin);
			if (found_version)
			{
				IHeapHdr	vh;

				ih_hdr_read(v.tuple, &vh);
				*found_version = iheap_version_urec(rel, &vh);
			}
			/* copy out while the content lock is held */
			iheap_store_version(slot, &v, true);
			if (v.mem)
				pfree(v.mem);
			ItemPointerCopy(tid, &slot->tts_tid);
			slot->tts_tableOid = RelationGetRelid(rel);
		}
	}
	if (!visible && all_dead)
		*all_dead = item_dead_to_all(rel, buf, page, off);

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	return visible;
}

IndexFetchTableData *
iheap_index_fetch_begin(Relation rel, uint32 flags)
{
	IHeapIndexFetchData *f = palloc0(sizeof(IHeapIndexFetchData));

	f->xs_base.rel = rel;
	f->xs_base.flags = flags;
	f->xs_cbuf = InvalidBuffer;
	f->xs_dm.index = NULL;
	(void) iheap_relcache(rel, false);
	return &f->xs_base;
}

void
iheap_index_fetch_reset(IndexFetchTableData *scan)
{
	IHeapIndexFetchData *f = (IHeapIndexFetchData *) scan;

	if (BufferIsValid(f->xs_cbuf))
	{
		ReleaseBuffer(f->xs_cbuf);
		f->xs_cbuf = InvalidBuffer;
	}
	f->xs_dm.index = NULL;
}

void
iheap_index_fetch_end(IndexFetchTableData *scan)
{
	iheap_index_fetch_reset(scan);
	pfree(scan);
}

bool
iheap_index_fetch_tuple(IndexFetchTableData *scan, ItemPointer tid,
						Snapshot snapshot, TupleTableSlot *slot,
						bool *call_again, bool *all_dead)
{
	IHeapIndexFetchData *f = (IHeapIndexFetchData *) scan;
	Relation	rel = f->xs_base.rel;
	BlockNumber blk = ItemPointerGetBlockNumber(tid);
	UndoRecPtr	version = InvalidUndoRecPtr;
	bool		found;

	*call_again = false;
	if (all_dead)
		*all_dead = false;

	if (!BufferIsValid(f->xs_cbuf) || BufferGetBlockNumber(f->xs_cbuf) != blk)
	{
		if (blk >= RelationGetNumberOfBlocks(rel))
			return false;
		f->xs_cbuf = ReleaseAndReadBuffer(f->xs_cbuf, rel, blk);
	}

	found = fetch_in_buffer(rel, f->xs_cbuf, tid, snapshot, slot, all_dead,
							false, f->xs_dm.index ? &f->xs_dm : NULL,
							snapshot->snapshot_type == SNAPSHOT_DIRTY ?
							&version : NULL);
	f->xs_dm.index = NULL;		/* each entry is good for one fetch */
	if (found)
	{
		pgstat_count_heap_fetch(rel);
		if (snapshot->snapshot_type == SNAPSHOT_DIRTY)
			note_dirty_found(rel, tid, version);
	}
	return found;
}

bool
iheap_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot,
						TupleTableSlot *slot)
{
	Buffer		buf;
	bool		found;

	if (ItemPointerGetBlockNumber(tid) >= RelationGetNumberOfBlocks(rel))
		return false;
	buf = ReadBufferExtended(rel, MAIN_FORKNUM, ItemPointerGetBlockNumber(tid),
							 RBM_NORMAL, NULL);
	/*
	 * The executor fetches the old tuple of an UPDATE (and the row of a
	 * trigger or RETURNING) with SnapshotAny, meaning the version at that
	 * TID: for us the newest settled one.
	 */
	found = fetch_in_buffer(rel, buf, tid, snapshot, slot, NULL,
							snapshot->snapshot_type == SNAPSHOT_ANY, NULL, NULL);
	ReleaseBuffer(buf);
	return found;
}

bool
iheap_tuple_tid_valid(TableScanDesc sscan, ItemPointer tid)
{
	IHeapScanDesc scan = (IHeapScanDesc) sscan;

	return ItemPointerIsValid(tid) &&
		ItemPointerGetBlockNumber(tid) < scan->rs_nblocks &&
		ItemPointerGetBlockNumber(tid) != IHEAP_METAPAGE_BLKNO;
}

/* follow MOVED stubs of committed (or our own) movers */
void
iheap_get_latest_tid(TableScanDesc sscan, ItemPointer tid)
{
	Relation	rel = sscan->rs_rd;
	int			steps = 0;

	while (steps++ < 1000)
	{
		Buffer		buf;
		Page		page;
		ItemId		lp;
		IHeapHdr	h;
		IHeapUndoRec *rec;
		IHeapXidStatus st;

		if (ItemPointerGetBlockNumber(tid) >= RelationGetNumberOfBlocks(rel))
			return;
		buf = ReadBufferExtended(rel, MAIN_FORKNUM, ItemPointerGetBlockNumber(tid),
								 RBM_NORMAL, NULL);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!IHeapPageIsData(page) ||
			ItemPointerGetOffsetNumber(tid) > PageGetMaxOffsetNumber(page))
		{
			UnlockReleaseBuffer(buf);
			return;
		}
		lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
		if (!ItemIdIsNormal(lp))
		{
			UnlockReleaseBuffer(buf);
			return;
		}
		ih_hdr_read((char *) page + ItemIdGetOffset(lp), &h);
		st = iheap_is_frozen(rel, &h) ? IHX_COMMITTED :
			iheap_xid_status(h.xid, buf, (char *) page + ItemIdGetOffset(lp));
		UnlockReleaseBuffer(buf);

		if (!(h.infomask & IHT_MOVED) || iheap_is_frozen(rel, &h) ||
			(st != IHX_COMMITTED && st != IHX_CURRENT))
			return;
		rec = iheap_undo_fetch_version_rec(rel, &h);
		if (rec == NULL || rec->hdr.ur_type != IHU_MOVE)
		{
			if (rec)
				pfree(rec);
			return;
		}
		memcpy(tid, rec->payload, sizeof(ItemPointerData));
		pfree(rec);
	}
}

bool
iheap_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
							   Snapshot snapshot)
{
	TupleTableSlot *tmp;
	bool		result;
	TransactionId xmin = TTS_IS_IHEAPTUPLE(slot) ?
		((IHeapTupleTableSlot *) slot)->xmin : InvalidTransactionId;

	tmp = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsIHeapTuple);
	result = iheap_fetch_row_version(rel, &slot->tts_tid, snapshot, tmp);
	if (result && TransactionIdIsValid(xmin))
		result = TransactionIdEquals(((IHeapTupleTableSlot *) tmp)->xmin, xmin);
	ExecDropSingleTupleTableSlot(tmp);
	return result;
}

/* ------------------------------------------------------------------------
 * Bitmap scans
 * ------------------------------------------------------------------------
 */
bool
iheap_scan_bitmap_next_tuple(TableScanDesc sscan, TupleTableSlot *slot,
							 bool *recheck, uint64 *lossy_pages,
							 uint64 *exact_pages)
{
	IHeapScanExt *ext = (IHeapScanExt *) sscan;
	IHeapScanDesc scan = &ext->d;

	while (scan->rs_cindex >= scan->rs_ntuples)
	{
		void	   *per_buffer_data;
		TBMIterateResult *tbmres;
		Buffer		buf;
		Page		page;
		OffsetNumber offsets[TBM_MAX_TUPLES_PER_PAGE];
		int			noffsets = -1;
		MemoryContext cxt;
		char	   *copy;

		scan->rs_cindex = 0;
		scan->rs_ntuples = 0;

		buf = read_stream_next_buffer(scan->rs_read_stream, &per_buffer_data);
		if (!BufferIsValid(buf))
			return false;
		tbmres = per_buffer_data;
		if (!tbmres->lossy)
			noffsets = tbm_extract_page_tuple(tbmres, offsets, TBM_MAX_TUPLES_PER_PAGE);
		scan->rs_bm_recheck = tbmres->recheck;
		scan->rs_cblock = tbmres->blockno;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (IHeapPageIsData(page))
		{
			copy = pages_next(&ext->pages, &cxt);
			scan->rs_page = copy;
			scan->rs_ntuples =
				iheap_collect_page_versions(scan->rs_base.rs_rd, buf,
											scan->rs_base.rs_snapshot, copy,
											scan->rs_visoff, scan->rs_vis, cxt,
											tbmres->lossy ? NULL : offsets,
											tbmres->lossy ? 0 : noffsets,
											true);
		}
		UnlockReleaseBuffer(buf);

		if (tbmres->lossy)
			(*lossy_pages)++;
		else
			(*exact_pages)++;
	}

	/*
	 * An older version of a row may have other values in the columns of a
	 * dmbtree index than its newest one, which index entries without a
	 * delete-mark match: the quals are rechecked.
	 */
	*recheck = scan->rs_bm_recheck || scan->rs_vis[scan->rs_cindex].from_undo;
	return_current(scan, slot);
	scan->rs_cindex++;
	pgstat_count_heap_fetch(scan->rs_base.rs_rd);
	return true;
}

/* ------------------------------------------------------------------------
 * Sample scans
 * ------------------------------------------------------------------------
 */
bool
iheap_scan_sample_next_block(TableScanDesc sscan, SampleScanState *scanstate)
{
	IHeapScanExt *ext = (IHeapScanExt *) sscan;
	IHeapScanDesc scan = &ext->d;
	TsmRoutine *tsm = scanstate->tsmroutine;
	BlockNumber blockno;

	for (;;)
	{
		Buffer		buf;
		Page		page;
		MemoryContext cxt;
		char	   *copy;

		if (scan->rs_nblocks == 0)
			return false;

		if (tsm->NextSampleBlock)
			blockno = tsm->NextSampleBlock(scanstate, scan->rs_nblocks);
		else if (!scan->rs_sample_inited)
		{
			blockno = 0;
			scan->rs_sample_inited = true;
		}
		else
		{
			blockno = scan->rs_sample_block + 1;
			if (blockno >= scan->rs_nblocks)
				blockno = InvalidBlockNumber;
		}

		scan->rs_sample_block = blockno;
		if (!BlockNumberIsValid(blockno))
		{
			scan->rs_sample_inited = false;
			return false;
		}

		CHECK_FOR_INTERRUPTS();
		buf = ReadBufferExtended(sscan->rs_rd, MAIN_FORKNUM, blockno, RBM_NORMAL,
								 scan->rs_strategy);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		scan->rs_cblock = blockno;
		scan->rs_ntuples = 0;
		scan->rs_cindex = 0;
		scan->rs_sample_maxoff = InvalidOffsetNumber;
		if (IHeapPageIsData(page))
		{
			scan->rs_sample_maxoff = PageGetMaxOffsetNumber(page);
			copy = pages_next(&ext->pages, &cxt);
			scan->rs_page = copy;
			scan->rs_ntuples =
				iheap_collect_page_versions(sscan->rs_rd, buf, sscan->rs_snapshot,
											copy, scan->rs_visoff, scan->rs_vis,
											cxt, NULL, 0, false);
		}
		UnlockReleaseBuffer(buf);
		return true;
	}
}

bool
iheap_scan_sample_next_tuple(TableScanDesc sscan, SampleScanState *scanstate,
							 TupleTableSlot *slot)
{
	IHeapScanDesc scan = (IHeapScanDesc) sscan;
	TsmRoutine *tsm = scanstate->tsmroutine;

	if (scan->rs_sample_maxoff == InvalidOffsetNumber)
	{
		ExecClearTuple(slot);
		return false;
	}

	for (;;)
	{
		OffsetNumber off;

		CHECK_FOR_INTERRUPTS();
		off = tsm->NextSampleTuple(scanstate, scan->rs_cblock,
								   scan->rs_sample_maxoff);
		if (!OffsetNumberIsValid(off))
		{
			ExecClearTuple(slot);
			return false;
		}
		for (int i = 0; i < scan->rs_ntuples; i++)
		{
			if (scan->rs_visoff[i] == off)
			{
				scan->rs_cindex = i;
				return_current(scan, slot);
				pgstat_count_heap_getnext(sscan->rs_rd);
				return true;
			}
		}
	}
}

/* ------------------------------------------------------------------------
 * ANALYZE
 * ------------------------------------------------------------------------
 */
bool
iheap_scan_analyze_next_block(TableScanDesc sscan, ReadStream *stream)
{
	IHeapScanExt *ext = (IHeapScanExt *) sscan;
	IHeapScanDesc scan = &ext->d;

	for (;;)
	{
		Buffer		buf = read_stream_next_buffer(stream, NULL);
		Page		page;
		MemoryContext cxt;
		char	   *copy;

		if (!BufferIsValid(buf))
			return false;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		scan->rs_cblock = BufferGetBlockNumber(buf);
		scan->rs_ntuples = 0;
		scan->rs_cindex = 0;
		ext->ndead = 0;
		if (IHeapPageIsData(page))
		{
			Relation	rel = sscan->rs_rd;
			OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
			MemoryContext oldcxt;
			int			n = 0;

			/*
			 * As heap's ANALYZE: classify each row by its current state, with
			 * no snapshot and no walk down version chains.  A committed row is
			 * sampled as it is on the page, a committed deletion counts as a
			 * dead row, and undo is read only for a row that a running (or
			 * aborted) transaction is changing, to sample the version before
			 * the change.  Resolving every row against ANALYZE's snapshot
			 * made sampling read undo for each row changed since, slowing it
			 * under cost-based delay while its snapshot held back discard.
			 */
			copy = pages_next(&ext->pages, &cxt);
			memcpy(copy, page, BLCKSZ);
			scan->rs_page = copy;
			oldcxt = MemoryContextSwitchTo(cxt);
			for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
			{
				ItemId		lp = PageGetItemId(page, off);
				char	   *tup;
				IHeapHdr	h;
				IHeapXidStatus st;
				IHeapVersion v;

				if (ItemIdIsDead(lp))
				{
					ext->ndead++;
					continue;
				}
				if (!ItemIdIsNormal(lp))
					continue;
				tup = (char *) page + ItemIdGetOffset(lp);
				ih_hdr_read(tup, &h);
				st = iheap_is_frozen(rel, &h) ? IHX_COMMITTED :
					iheap_xid_status(h.xid, buf, tup);
				if (st == IHX_COMMITTED || st == IHX_CURRENT)
				{
					if (h.infomask & (IHT_DELETED | IHT_MOVED))
					{
						ext->ndead++;
						continue;
					}
					v.tuple = copy + ItemIdGetOffset(lp);
					v.len = ItemIdGetLength(lp);
					v.mem = NULL;
					v.from_undo = false;
					v.xmin = h.xid;
					v.xmax = InvalidTransactionId;
					v.cmin = InvalidCommandId;
				}
				else
				{
					/* the newest settled version: before the running change */
					if (!iheap_resolve_version_ext(rel, buf, tup,
												   ItemIdGetLength(lp),
												   copy + ItemIdGetOffset(lp),
												   SnapshotAny, &v, true, false))
						continue;	/* inserted by a running transaction */
					if (TransactionIdIsValid(v.xmax))
					{
						ext->ndead++;
						continue;
					}
				}
				ensure_aligned(&v, cxt);
				scan->rs_visoff[n] = off;
				scan->rs_vis[n] = v;
				n++;
			}
			MemoryContextSwitchTo(oldcxt);
			scan->rs_ntuples = n;
		}
		UnlockReleaseBuffer(buf);
		return true;
	}
}

bool
iheap_scan_analyze_next_tuple(TableScanDesc sscan, double *liverows,
							  double *deadrows, TupleTableSlot *slot)
{
	IHeapScanExt *ext = (IHeapScanExt *) sscan;
	IHeapScanDesc scan = &ext->d;

	if (scan->rs_cindex < scan->rs_ntuples)
	{
		return_current(scan, slot);
		scan->rs_cindex++;
		*liverows += 1;
		return true;
	}

	*deadrows += ext->ndead;
	ext->ndead = 0;
	ExecClearTuple(slot);
	return false;
}
