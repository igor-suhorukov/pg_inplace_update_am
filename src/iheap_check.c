/*-------------------------------------------------------------------------
 *
 * iheap_check.c
 *	  iheap_check(): verifying the structure of an iheap relation
 *	  (DESIGN.md §15, §21 D35).
 *
 * Each problem found is returned as a row (block, offset, message), as
 * amcheck's verify_heapam() does, instead of an error at the first one:
 *
 *	- the metapage: its format, the bounds of the undo stream, and the
 *	  extents (no overlaps; the live ones map every logical undo page from
 *	  the discard pointer to the next page to allocate exactly once);
 *	- every other page: its header, and its type against the extent map
 *	  (data pages lie outside extents; the live part of the stream is undo
 *	  pages carrying the logical page number their extent gives them);
 *	- data pages: line pointers, tuple headers and the layout of the
 *	  attributes, the undo chains of the tuples that are not frozen, and the
 *	  visibility map bit;
 *	- the undo pages of the live part of the stream: every record, the page
 *	  header's record count and xid range;
 *	- with check_indexes, that the btree and dmbtree indexes of the relation
 *	  point only to used line pointers of data pages.
 *
 * Like verify_heapam(), it takes an AccessShareLock and reads one page at a
 * time, from a copy, so writers and VACUUM go on meanwhile.  What they can
 * make look wrong is looked at again before it is reported: undo that the
 * discard pointer passed meanwhile, or an extent dropped (the metapage is
 * read again); an index entry that VACUUM removed meanwhile (the leaf page
 * stays locked while the line pointer is looked at again).
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/transam.h"
#include "access/visibilitymap.h"
#include "access/xlog.h"
#include "catalog/pg_am_d.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"
#include "varatt.h"

#include "iheap.h"
#include "dmbtree_api.h"

PG_FUNCTION_INFO_V1(iheap_check);

#define CHAIN_MAX_STEPS		100000

/* every infomask flag iheap defines */
#define IHT_KNOWN_FLAGS \
	(IHT_HASNULL | IHT_HASVARWIDTH | IHT_HASEXTERNAL | IHT_ALIGNDATA | \
	 IHT_XID_COMMITTED | IHT_XID_ABORTED | IHT_DELETED | IHT_MOVED | \
	 IHT_LOCKED | IHT_INPLACE | IHT_SPECULATIVE | IHT_STUB | \
	 IHT_KEYS_UPDATED | IHT_MOVED_PART | IHT_COMPACT)

/* flags of versions made by a transaction some snapshot may not see yet */
#define IHT_XID_FLAGS \
	(IHT_XID_HINTS | IHT_DELETED | IHT_MOVED | IHT_INPLACE | IHT_SPECULATIVE | \
	 IHT_STUB | IHT_KEYS_UPDATED | IHT_MOVED_PART)

#define IHUF_KNOWN_FLAGS \
	(IHUF_SPEC | IHUF_LOCKERS | IHUF_KEYS_UPDATED | IHUF_MOVED_IN)

/* the line pointers of a block, for the index cross-check */
typedef struct BlockMap
{
	bool		data;			/* a data page */
	uint16		maxoff;
	uint8		used[(IHEAP_MAX_ITEMS + 7) / 8];	/* LP_NORMAL or LP_DEAD */
} BlockMap;

typedef struct IHCheckState
{
	Relation	rel;
	ReturnSetInfo *rsinfo;
	MemoryContext cxt;			/* lives as long as the check */
	MemoryContext pagecxt;		/* reset for each page */
	BufferAccessStrategy bstrategy;
	BlockNumber nblocks;
	TupleDesc	desc;
	Oid			toastrelid;
	TransactionId relfrozenxid;
	TransactionId nextxid;		/* read again before a xid is called future */

	/* the undo stream, as the metapage last told it */
	UndoRecPtr	discard;
	uint64		next_lpage;
	int			nextents;
	IHeapExtent *extents;		/* sorted by physical start */

	/* index cross-check: the line pointers of the blocks of one range */
	List	   *indexes;		/* open btree and dmbtree indexes */
	BlockNumber map_start;
	BlockNumber map_end;
	BlockMap   *map;
} IHCheckState;

/* ------------------------------------------------------------------------
 * Reporting
 * ------------------------------------------------------------------------
 */
static void report(IHCheckState *cs, BlockNumber blk, OffsetNumber off,
				   const char *fmt,...) pg_attribute_printf(4, 5);

static void
report(IHCheckState *cs, BlockNumber blk, OffsetNumber off, const char *fmt,...)
{
	Datum		values[3];
	bool		nulls[3] = {false, false, false};
	StringInfoData buf;
	MemoryContext oldcxt = MemoryContextSwitchTo(cs->pagecxt);

	initStringInfo(&buf);
	for (;;)
	{
		va_list		args;
		int			needed;

		va_start(args, fmt);
		needed = appendStringInfoVA(&buf, fmt, args);
		va_end(args);
		if (needed == 0)
			break;
		enlargeStringInfo(&buf, needed);
	}

	values[0] = Int64GetDatum((int64) blk);
	if (OffsetNumberIsValid(off))
		values[1] = Int32GetDatum(off);
	else
		nulls[1] = true;
	values[2] = CStringGetTextDatum(buf.data);
	tuplestore_putvalues(cs->rsinfo->setResult, cs->rsinfo->setDesc, values,
						 nulls);
	MemoryContextSwitchTo(oldcxt);
}

/* what is wrong with xid as the writer of a live undo record, or NULL */
static const char *
xid_problem(IHCheckState *cs, TransactionId xid)
{
	if (!TransactionIdIsNormal(xid))
		return "is not a normal xid";
	if (TransactionIdIsNormal(cs->relfrozenxid) &&
		TransactionIdPrecedes(xid, cs->relfrozenxid))
		return "precedes the relation's relfrozenxid";
	if (TransactionIdFollowsOrEquals(xid, cs->nextxid))
	{
		cs->nextxid = ReadNextTransactionId();
		if (TransactionIdFollowsOrEquals(xid, cs->nextxid))
			return "is in the future";
	}
	return NULL;
}

/* ------------------------------------------------------------------------
 * The metapage and the extent map
 * ------------------------------------------------------------------------
 */
static int
extent_pstart_cmp(const void *a, const void *b)
{
	const IHeapExtent *ea = (const IHeapExtent *) a;
	const IHeapExtent *eb = (const IHeapExtent *) b;

	if (ea->ie_pstart != eb->ie_pstart)
		return ea->ie_pstart < eb->ie_pstart ? -1 : 1;
	return 0;
}

static int
extent_lstart_cmp(const void *a, const void *b)
{
	const IHeapExtent *ea = *(const IHeapExtent *const *) a;
	const IHeapExtent *eb = *(const IHeapExtent *const *) b;

	if (ea->ie_lstart != eb->ie_lstart)
		return ea->ie_lstart < eb->ie_lstart ? -1 : 1;
	return 0;
}

static void
read_block_copy(IHCheckState *cs, BlockNumber blk, char *dst)
{
	Buffer		buf;

	buf = ReadBufferExtended(cs->rel, MAIN_FORKNUM, blk, RBM_NORMAL,
							 cs->bstrategy);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	memcpy(dst, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
}

/* check the extents of the metapage just read (cs) */
static void
check_extents(IHCheckState *cs, uint64 discard_lpage)
{
	IHeapExtent **live = palloc(Max(cs->nextents, 1) * sizeof(IHeapExtent *));
	int			nlive = 0;
	uint64		covered = discard_lpage;	/* mapped up to here */

	for (int i = 0; i < cs->nextents; i++)
	{
		IHeapExtent *e = &cs->extents[i];

		if (e->ie_npages < IHEAP_EXTENT_MIN_PAGES ||
			e->ie_npages > IHEAP_EXTENT_MAX_PAGES ||
			(e->ie_npages & (e->ie_npages - 1)) != 0)
			report(cs, IHEAP_METAPAGE_BLKNO, InvalidOffsetNumber,
				   "extent at block %u has %u pages, not a power of two from %d to %d",
				   e->ie_pstart, e->ie_npages, IHEAP_EXTENT_MIN_PAGES,
				   IHEAP_EXTENT_MAX_PAGES);
		if (e->ie_pstart == IHEAP_METAPAGE_BLKNO ||
			e->ie_pstart > MaxBlockNumber - e->ie_npages)
			report(cs, IHEAP_METAPAGE_BLKNO, InvalidOffsetNumber,
				   "extent at block %u of %u pages is out of range",
				   e->ie_pstart, e->ie_npages);
		if (i > 0 &&
			cs->extents[i - 1].ie_pstart + cs->extents[i - 1].ie_npages > e->ie_pstart)
			report(cs, IHEAP_METAPAGE_BLKNO, InvalidOffsetNumber,
				   "extents at blocks %u and %u overlap",
				   cs->extents[i - 1].ie_pstart, e->ie_pstart);
		if (e->ie_lstart == IHEAP_EXTENT_FREE)
			continue;
		live[nlive++] = e;

		/* discard frees an extent with its last page (iheap_meta_free_extents) */
		if (e->ie_lstart + e->ie_npages <= discard_lpage)
			report(cs, IHEAP_METAPAGE_BLKNO, InvalidOffsetNumber,
				   "live extent at block %u holds only discarded undo pages " UINT64_FORMAT " to " UINT64_FORMAT,
				   e->ie_pstart, e->ie_lstart, e->ie_lstart + e->ie_npages - 1);
	}

	/* the live extents map each page of [discard, next) exactly once */
	qsort(live, nlive, sizeof(IHeapExtent *), extent_lstart_cmp);
	for (int i = 0; i < nlive; i++)
	{
		IHeapExtent *e = live[i];

		if (i > 0 &&
			live[i - 1]->ie_lstart + live[i - 1]->ie_npages > e->ie_lstart)
			report(cs, IHEAP_METAPAGE_BLKNO, InvalidOffsetNumber,
				   "live extents at blocks %u and %u both hold undo page " UINT64_FORMAT,
				   live[i - 1]->ie_pstart, e->ie_pstart, e->ie_lstart);
		if (e->ie_lstart > covered && covered < cs->next_lpage)
			report(cs, IHEAP_METAPAGE_BLKNO, InvalidOffsetNumber,
				   "no extent holds undo pages " UINT64_FORMAT " to " UINT64_FORMAT,
				   covered, Min(e->ie_lstart, cs->next_lpage) - 1);
		covered = Max(covered, e->ie_lstart + e->ie_npages);
	}
	if (covered < cs->next_lpage)
		report(cs, IHEAP_METAPAGE_BLKNO, InvalidOffsetNumber,
			   "no extent holds undo pages " UINT64_FORMAT " to " UINT64_FORMAT,
			   covered, cs->next_lpage - 1);
	pfree(live);
}

/*
 * Take the undo stream's bounds and extents from the metapage; check them
 * when "check" (the first time).  False if there is no usable metapage.
 */
static bool
read_metapage(IHCheckState *cs, bool check)
{
	PGIOAlignedBlock page;
	IHeapMetaPageData *meta;
	uint32		nextents;
	uint64		discard_lpage;

	read_block_copy(cs, IHEAP_METAPAGE_BLKNO, page.data);
	if (IHeapPageGetType(page.data) != IHEAP_PAGE_META)
	{
		if (check)
			report(cs, IHEAP_METAPAGE_BLKNO, InvalidOffsetNumber,
				   "block 0 is not a metapage");
		return false;
	}
	meta = IHeapPageGetMeta(page.data);
	if (meta->im_magic != IHEAP_META_MAGIC ||
		meta->im_version != IHEAP_META_VERSION)
	{
		if (check)
			report(cs, IHEAP_METAPAGE_BLKNO, InvalidOffsetNumber,
				   "metapage has magic %08X and version %u, expected %08X and %u",
				   meta->im_magic, meta->im_version, IHEAP_META_MAGIC,
				   IHEAP_META_VERSION);
		return false;
	}

	nextents = meta->im_nextents;
	if (nextents > (uint32) IHEAP_MAX_EXTENTS)
	{
		if (check)
			report(cs, IHEAP_METAPAGE_BLKNO, InvalidOffsetNumber,
				   "metapage lists %u extents, at most %d fit",
				   nextents, IHEAP_MAX_EXTENTS);
		nextents = IHEAP_MAX_EXTENTS;
	}

	cs->discard = meta->im_discard;
	cs->next_lpage = meta->im_next_lpage;
	if (cs->extents)
		pfree(cs->extents);
	cs->extents = MemoryContextAlloc(cs->cxt,
									 Max(nextents, 1) * sizeof(IHeapExtent));
	memcpy(cs->extents, meta->im_extents, nextents * sizeof(IHeapExtent));
	cs->nextents = nextents;
	qsort(cs->extents, nextents, sizeof(IHeapExtent), extent_pstart_cmp);

	if (!check)
		return true;

	discard_lpage = UndoRecPtrGetLPage(meta->im_discard);
	if (((PageHeader) page.data)->pd_lower !=
		(char *) &meta->im_extents[nextents] - page.data)
		report(cs, IHEAP_METAPAGE_BLKNO, InvalidOffsetNumber,
			   "metapage's pd_lower %u is not the end of its extent list",
			   ((PageHeader) page.data)->pd_lower);
	if (UndoRecPtrGetOffset(meta->im_discard) != 0 || discard_lpage < 1)
		report(cs, IHEAP_METAPAGE_BLKNO, InvalidOffsetNumber,
			   "discard pointer " UINT64_FORMAT " is not the start of an undo page",
			   meta->im_discard);
	if (meta->im_next_lpage < 1 || discard_lpage > meta->im_next_lpage)
		report(cs, IHEAP_METAPAGE_BLKNO, InvalidOffsetNumber,
			   "discard pointer " UINT64_FORMAT " is beyond the next undo page " UINT64_FORMAT,
			   meta->im_discard, meta->im_next_lpage);
	check_extents(cs, discard_lpage);
	return true;
}

/* read the metapage again: the discard pointer and the extents move on */
static void
refresh_metapage(IHCheckState *cs)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(cs->cxt);

	(void) read_metapage(cs, false);
	MemoryContextSwitchTo(oldcxt);
}

/* the extent holding block blk, or NULL */
static IHeapExtent *
block_extent(IHCheckState *cs, BlockNumber blk)
{
	int			lo = 0;
	int			hi = cs->nextents - 1;

	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;
		IHeapExtent *e = &cs->extents[mid];

		if (blk < e->ie_pstart)
			hi = mid - 1;
		else if (blk >= e->ie_pstart + e->ie_npages)
			lo = mid + 1;
		else
			return e;
	}
	return NULL;
}

/* the block of logical undo page lpage, or InvalidBlockNumber */
static BlockNumber
lpage_block(IHCheckState *cs, uint64 lpage)
{
	for (int i = 0; i < cs->nextents; i++)
	{
		IHeapExtent *e = &cs->extents[i];

		if (e->ie_lstart != IHEAP_EXTENT_FREE && lpage >= e->ie_lstart &&
			lpage < e->ie_lstart + e->ie_npages)
			return e->ie_pstart + (BlockNumber) (lpage - e->ie_lstart);
	}
	return InvalidBlockNumber;
}

/* ------------------------------------------------------------------------
 * Undo records
 * ------------------------------------------------------------------------
 */

/*
 * Read the undo record at urp into the page context.  Returns false with
 * *problem set if it is not there, or with *problem NULL if the discard
 * pointer passed it meanwhile (a record behind it is no longer needed).
 */
static bool
read_undo_record(IHCheckState *cs, UndoRecPtr urp, IHeapUndoRecHeader *hdr,
				 char **payload, uint32 *payload_len, const char **problem)
{
	uint64		lpage = UndoRecPtrGetLPage(urp);
	uint32		off = UndoRecPtrGetOffset(urp);
	BlockNumber blk;
	Buffer		buf;
	Page		page;
	bool		ok = false;

	*problem = NULL;
	if (off < IHEAP_UNDO_DATA_START || off >= BLCKSZ - IHEAP_SPECIAL_SIZE)
	{
		*problem = "points outside the records of an undo page";
		return false;
	}

	for (int attempt = 0; attempt < 2; attempt++)
	{
		if (attempt > 0)
		{
			/* the stream moved on, or the record is really missing */
			refresh_metapage(cs);
			if (urp < cs->discard)
				return false;
		}
		blk = lpage_block(cs, lpage);
		if (!BlockNumberIsValid(blk) ||
			blk >= RelationGetNumberOfBlocks(cs->rel))
			continue;

		buf = ReadBufferExtended(cs->rel, MAIN_FORKNUM, blk, RBM_NORMAL,
								 cs->bstrategy);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (IHeapPageGetType(page) == IHEAP_PAGE_UNDO &&
			IHeapUndoPageGetHeader(page)->uph_lpage == lpage &&
			((PageHeader) page)->pd_lower <= BLCKSZ - IHEAP_SPECIAL_SIZE)
		{
			const char *p;

			if (iheap_undo_read_record(page, off, ((PageHeader) page)->pd_lower,
									   hdr, &p, payload_len))
			{
				*payload = MemoryContextAlloc(cs->pagecxt,
											  Max(*payload_len, 1));
				memcpy(*payload, p, *payload_len);
				ok = true;
			}
		}
		UnlockReleaseBuffer(buf);
		if (ok)
			return true;
	}

	*problem = "is not in the undo stream";
	return false;
}

/* the payload of a record without its trailing lockers list, if valid */
static bool
payload_core_len(const IHeapUndoRecHeader *hdr, const char *payload,
				 uint32 payload_len, uint32 *core, int *nlockers)
{
	uint16		n;

	*nlockers = 0;
	*core = payload_len;
	if ((hdr->ur_flags & IHUF_LOCKERS) == 0)
		return true;
	if (payload_len < sizeof(uint16))
		return false;
	memcpy(&n, payload + payload_len - sizeof(uint16), sizeof(uint16));
	if (n > IHEAP_MAX_LOCKERS || iheap_lockers_size(n) > payload_len)
		return false;
	*nlockers = n;
	*core = payload_len - iheap_lockers_size(n);
	return true;
}

/* does a record of block blk describe item "off"? */
static bool
record_targets(const IHeapUndoRecHeader *hdr, const char *payload,
			   uint32 core, OffsetNumber off)
{
	uint16		n;

	if (hdr->ur_type != IHU_MULTI_INSERT)
		return hdr->ur_offnum == off;
	if (core < sizeof(uint16))
		return false;
	memcpy(&n, payload, sizeof(uint16));
	if (sizeof(uint16) + n * sizeof(OffsetNumber) > core)
		return false;
	for (int i = 0; i < n; i++)
	{
		OffsetNumber o;

		memcpy(&o, payload + sizeof(uint16) + i * sizeof(OffsetNumber),
			   sizeof(OffsetNumber));
		if (o == off)
			return true;
	}
	return false;
}

static void
account_xid(TransactionId xid, TransactionId *minxid, TransactionId *maxxid)
{
	if (!TransactionIdIsNormal(xid))
		return;
	if (!TransactionIdIsValid(*minxid) || TransactionIdPrecedes(xid, *minxid))
		*minxid = xid;
	if (!TransactionIdIsValid(*maxxid) || TransactionIdFollows(xid, *maxxid))
		*maxxid = xid;
}

/*
 * What is wrong with the record at urp, of an undo page of the live part of
 * the stream, or NULL.  Accounts the xids its page header must cover, as
 * the undo layer does (undo_page_account()).
 */
static const char *
undo_record_problem(IHCheckState *cs, UndoRecPtr urp,
					const IHeapUndoRecHeader *hdr, const char *payload,
					uint32 payload_len, TransactionId *minxid,
					TransactionId *maxxid)
{
	uint32		core;
	int			nlockers;
	const char *xp;

	account_xid(hdr->ur_xid, minxid, maxxid);
	if (hdr->ur_type == IHU_LOCK)
	{
		IHeapHdr	ph;

		/* the creator of a version frozen already is never looked up */
		ih_hdr_read(hdr->ur_prev_hdr, &ph);
		if (UndoRecPtrIsValid(ph.urec))
			account_xid(ph.xid, minxid, maxxid);
	}
	if (!payload_core_len(hdr, payload, payload_len, &core, &nlockers))
		return "has a malformed lockers list";
	for (int i = 0; i < nlockers; i++)
	{
		const char *p = payload + core + i * IHEAP_LOCKER_SIZE;
		TransactionId xid;

		memcpy(&xid, p, sizeof(TransactionId));
		account_xid(xid, minxid, maxxid);
		if ((uint8) p[sizeof(TransactionId)] > LockTupleExclusive)
			return psprintf("has a locker with lock mode %u",
							(uint8) p[sizeof(TransactionId)]);
		if ((xp = xid_problem(cs, xid)) != NULL)
			return psprintf("has a locker whose xid %u %s", xid, xp);
	}

	if (hdr->ur_type < IHU_INSERT || hdr->ur_type > IHU_LOCK)
		return psprintf("has type %u", hdr->ur_type);
	if (hdr->ur_flags & ~IHUF_KNOWN_FLAGS)
		return psprintf("has flags %02X", hdr->ur_flags);
	if ((xp = xid_problem(cs, hdr->ur_xid)) != NULL)
		return psprintf("was written by xid %u, which %s", hdr->ur_xid, xp);
	if (hdr->ur_blkno == IHEAP_METAPAGE_BLKNO)
		return "is about block 0, the metapage";
	if (hdr->ur_type != IHU_MULTI_INSERT &&
		(hdr->ur_offnum < FirstOffsetNumber || hdr->ur_offnum > IHEAP_MAX_ITEMS))
		return psprintf("is about offset %u", hdr->ur_offnum);
	if (UndoRecPtrIsValid(hdr->ur_xact_prev) && hdr->ur_xact_prev >= urp)
		return psprintf("has the transaction's previous record at " UINT64_FORMAT ", not before it",
						hdr->ur_xact_prev);

	switch (hdr->ur_type)
	{
		case IHU_INSERT:
		case IHU_MULTI_INSERT:
			{
				static const char zero[IHEAP_TUPHDR_SIZE] = {0};

				if (memcmp(hdr->ur_prev_hdr, zero, IHEAP_TUPHDR_SIZE) != 0)
					return "is an insert with a previous tuple header";
				if (hdr->ur_type == IHU_INSERT)
				{
					if (core != ((hdr->ur_flags & IHUF_SPEC) ? sizeof(uint32) : 0))
						return psprintf("is an insert with %u payload bytes", core);
				}
				else
				{
					uint16		n;

					if (core < sizeof(uint16))
						return "is a multi-insert without offsets";
					memcpy(&n, payload, sizeof(uint16));
					if (core != sizeof(uint16) + n * sizeof(OffsetNumber))
						return psprintf("is a multi-insert of %u offsets in %u bytes",
										n, core);
					for (int i = 0; i < n; i++)
					{
						OffsetNumber o;

						memcpy(&o, payload + sizeof(uint16) + i * sizeof(OffsetNumber),
							   sizeof(OffsetNumber));
						if (o < FirstOffsetNumber || o > IHEAP_MAX_ITEMS)
							return psprintf("is a multi-insert of offset %u", o);
					}
				}
				break;
			}
		case IHU_UPDATE:
			{
				IHeapUpdateDelta d;
				IHeapHdr	ph;

				if (core < sizeof(d))
					return "is an update without a delta";
				memcpy(&d, payload, sizeof(d));
				ih_hdr_read(hdr->ur_prev_hdr, &ph);
				if (sizeof(d) + d.mid_len != core ||
					(uint32) d.prefix + d.suffix + d.mid_len + ih_hdr_size_of(&ph) != d.old_len)
					return psprintf("is an update with a malformed delta (old length %u, prefix %u, suffix %u, middle %u, payload %u)",
									d.old_len, d.prefix, d.suffix, d.mid_len, core);
				break;
			}
		case IHU_MOVE:
			{
				ItemPointerData newtid;

				if (core < sizeof(ItemPointerData))
					return "is a move without the new TID";
				memcpy(&newtid, payload, sizeof(ItemPointerData));
				if (ItemPointerGetBlockNumberNoCheck(&newtid) == IHEAP_METAPAGE_BLKNO ||
					ItemPointerGetOffsetNumberNoCheck(&newtid) < FirstOffsetNumber ||
					ItemPointerGetOffsetNumberNoCheck(&newtid) > IHEAP_MAX_ITEMS)
					return psprintf("is a move to (%u,%u)",
									ItemPointerGetBlockNumberNoCheck(&newtid),
									ItemPointerGetOffsetNumberNoCheck(&newtid));
				break;
			}
		case IHU_LOCK:
			if ((hdr->ur_flags & IHUF_LOCKERS) == 0 || nlockers == 0)
				return "is a lock without lockers";
			if (core != 0)
				return psprintf("is a lock with %u more payload bytes", core);
			break;
		default:
			/* IHU_DELETE: the old body */
			break;
	}
	return NULL;
}

/* ------------------------------------------------------------------------
 * Undo pages
 * ------------------------------------------------------------------------
 */
static void
check_undo_page(IHCheckState *cs, BlockNumber blk, Page page, uint64 lpage)
{
	IHeapUndoPageHeaderData *uph = IHeapUndoPageGetHeader(page);
	uint32		lower = ((PageHeader) page)->pd_lower;
	uint32		off = IHEAP_UNDO_DATA_START;
	int			nrecs = 0;
	TransactionId minxid = InvalidTransactionId;
	TransactionId maxxid = InvalidTransactionId;

	if (lower < IHEAP_UNDO_DATA_START || lower > BLCKSZ - IHEAP_SPECIAL_SIZE)
	{
		report(cs, blk, InvalidOffsetNumber,
			   "undo page's pd_lower %u is outside its record space", lower);
		return;
	}
	if (uph->uph_format != IHEAP_UNDO_FORMAT_V1 &&
		uph->uph_format != IHEAP_UNDO_FORMAT_COMPACT)
	{
		report(cs, blk, InvalidOffsetNumber,
			   "undo page has record format %u", uph->uph_format);
		return;
	}

	while (off < lower)
	{
		IHeapUndoRecHeader hdr;
		UndoRecPtr	urp = MakeUndoRecPtr(lpage, off);
		const char *payload;
		uint32		payload_len;
		const char *problem;

		if (!iheap_undo_read_record(page, off, lower, &hdr, &payload,
									&payload_len))
		{
			report(cs, blk, InvalidOffsetNumber,
				   "undo record " UINT64_FORMAT " does not fit the page",
				   urp);
			return;
		}
		problem = undo_record_problem(cs, urp, &hdr, payload, payload_len,
									  &minxid, &maxxid);
		if (problem)
			report(cs, blk, InvalidOffsetNumber, "undo record " UINT64_FORMAT " %s",
				   urp, problem);
		nrecs++;
		off += hdr.ur_len;
	}

	if (nrecs != uph->uph_nrecs)
		report(cs, blk, InvalidOffsetNumber,
			   "undo page header counts %u records, the page has %d",
			   uph->uph_nrecs, nrecs);
	else if (nrecs > 0 &&
			 (minxid != uph->uph_min_xid || maxxid != uph->uph_max_xid))
		report(cs, blk, InvalidOffsetNumber,
			   "undo page header says its records' xids range from %u to %u, they range from %u to %u",
			   uph->uph_min_xid, uph->uph_max_xid, minxid, maxxid);
}

/* ------------------------------------------------------------------------
 * Tuples
 * ------------------------------------------------------------------------
 */

/*
 * Walk the attributes of a tuple body as ih_deform_tuple() does, checking
 * that each value lies inside the tuple and that the values fill it.
 * Returns what is wrong, or NULL.
 */
static char *
attributes_problem(IHCheckState *cs, const char *tup, uint32 len,
				   const IHeapHdr *h)
{
	int			natts = h->infomask2 & IHT_NATTS_MASK;
	bool		hasnull = (h->infomask & IHT_HASNULL) != 0;
	const uint8 *bits = (const uint8 *) (tup + ih_hdr_size_of(h));
	const char *tp = tup + h->hoff;
	uint32		datalen = len - h->hoff;
	uint32		off = 0;
	bool		hasexternal = false;

	for (int i = 0; i < natts; i++)
	{
		CompactAttribute *att = TupleDescCompactAttr(cs->desc, i);

		if (hasnull && att_isnull(i, bits))
			continue;

		if (att->attbyval)
		{
			if (off + att->attlen > datalen)
				return psprintf("attribute %d ends beyond the tuple", i + 1);
			off += att->attlen;
		}
		else if (att->attlen == -1)
		{
			union
			{
				uint32		align;
				uint8		bytes[VARHDRSZ_EXTERNAL + sizeof(varatt_external)];
			}			hdr;
			uint8	   *b = hdr.bytes;
			uint32		size;

			if (off >= datalen)
				return psprintf("attribute %d starts beyond the tuple", i + 1);
			if (!VARATT_IS_SHORT(tp + off))
				off = TYPEALIGN(att->attalignby, off);
			if (off >= datalen)
				return psprintf("attribute %d starts beyond the tuple", i + 1);
			/* the value's header, aligned; what the tuple lacks reads as 0 */
			memset(&hdr, 0, sizeof(hdr));
			memcpy(b, tp + off, Min(sizeof(hdr.bytes), datalen - off));

			if (VARATT_IS_EXTERNAL(b))
			{
				varatt_external ext;

				if (VARTAG_EXTERNAL(b) != VARTAG_ONDISK)
					return psprintf("attribute %d is an external value of kind %u, which is never stored",
									i + 1, VARTAG_EXTERNAL(b));
				if (off + VARHDRSZ_EXTERNAL + sizeof(varatt_external) > datalen)
					return psprintf("attribute %d ends beyond the tuple", i + 1);
				memcpy(&ext, b + VARHDRSZ_EXTERNAL, sizeof(ext));
				if (ext.va_toastrelid != cs->toastrelid)
					return psprintf("attribute %d is stored in relation %u, not in the TOAST table %u",
									i + 1, ext.va_toastrelid, cs->toastrelid);
				if (ext.va_rawsize < VARHDRSZ ||
					VARATT_EXTERNAL_GET_EXTSIZE(ext) > (Size) (ext.va_rawsize - VARHDRSZ))
					return psprintf("attribute %d is an external value of %d bytes stored in %zu",
									i + 1, ext.va_rawsize,
									(Size) VARATT_EXTERNAL_GET_EXTSIZE(ext));
				hasexternal = true;
				size = VARHDRSZ_EXTERNAL + sizeof(varatt_external);
			}
			else if (VARATT_IS_SHORT(b))
				size = VARSIZE_SHORT(b);
			else
			{
				if (off + VARHDRSZ > datalen)
					return psprintf("attribute %d ends beyond the tuple", i + 1);
				size = VARSIZE(b);
				if (size < VARHDRSZ)
					return psprintf("attribute %d has length %u", i + 1, size);
			}
			if (size < 1 || off + size > datalen)
				return psprintf("attribute %d of %u bytes ends beyond the tuple",
								i + 1, size);
			off += size;
		}
		else if (att->attlen == -2)
		{
			const char *end;

			if (off >= datalen ||
				(end = memchr(tp + off, '\0', datalen - off)) == NULL)
				return psprintf("attribute %d is a string not ended inside the tuple",
								i + 1);
			off = (end - tp) + 1;
		}
		else
		{
			off = TYPEALIGN(att->attalignby, off);
			if (off + att->attlen > datalen)
				return psprintf("attribute %d ends beyond the tuple", i + 1);
			off += att->attlen;
		}
	}

	if (off != datalen)
		return psprintf("the attributes take %u of the %u data bytes", off,
						datalen);
	if (hasexternal != ((h->infomask & IHT_HASEXTERNAL) != 0))
		return psprintf("HASEXTERNAL is %s, but %s value is stored out of line",
						hasexternal ? "not set" : "set",
						hasexternal ? "a" : "no");
	return NULL;
}

/*
 * The undo chain of the item at (blk, off), whose current header is h and
 * length len (a stub has no body).  From the first record that is not
 * discarded down to the version a creating record ends: each record exists,
 * is about this item, and was written by the version's creator (a LOCK
 * record by a locker, keeping the creator).  A chain need not go back in the
 * stream: each backend appends to undo pages of its own (§21 D4), so an
 * older change of a row may sit on a newer page.  Only the length of a
 * chain bounds the walk.
 */
static void
check_undo_chain(IHCheckState *cs, BlockNumber blk, OffsetNumber off,
				 IHeapHdr h, uint32 len, bool stub)
{
	uint32		curlen = len;	/* length of the version h describes */
	bool		lenknown = !stub;
	bool		onpage = true;	/* h describes the item's version on the page */
	int			steps = 0;

	while (h.urec >= cs->discard)
	{
		IHeapUndoRecHeader rh;
		char	   *payload;
		uint32		payload_len;
		uint32		core;
		int			nlockers;
		const char *problem;
		const char *xp;
		IHeapHdr	ph;

		if (++steps > CHAIN_MAX_STEPS)
		{
			report(cs, blk, off, "undo chain is longer than %d records",
				   CHAIN_MAX_STEPS);
			return;
		}
		if (UndoRecPtrGetOffset(h.urec) < IHEAP_UNDO_DATA_START)
		{
			report(cs, blk, off, "undo pointer " UINT64_FORMAT " does not point to a record",
				   h.urec);
			return;
		}
		if (UndoRecPtrGetLPage(h.urec) >= cs->next_lpage)
		{
			refresh_metapage(cs);
			if (UndoRecPtrGetLPage(h.urec) >= cs->next_lpage)
			{
				report(cs, blk, off, "undo pointer " UINT64_FORMAT " is beyond the end of the undo stream",
					   h.urec);
				return;
			}
		}
		if (!read_undo_record(cs, h.urec, &rh, &payload, &payload_len,
							  &problem))
		{
			if (problem)
				report(cs, blk, off, "undo record " UINT64_FORMAT " %s",
					   h.urec, problem);
			return;
		}
		if (!payload_core_len(&rh, payload, payload_len, &core, &nlockers))
		{
			report(cs, blk, off, "undo record " UINT64_FORMAT " has a malformed lockers list",
				   h.urec);
			return;
		}
		if (rh.ur_blkno != blk || !record_targets(&rh, payload, core, off))
		{
			report(cs, blk, off, "undo record " UINT64_FORMAT " is about (%u,%u), not this item",
				   h.urec, rh.ur_blkno, rh.ur_offnum);
			return;
		}
		if ((xp = xid_problem(cs, rh.ur_xid)) != NULL)
		{
			report(cs, blk, off, "undo record " UINT64_FORMAT " was written by xid %u, which %s",
				   h.urec, rh.ur_xid, xp);
			return;
		}
		if ((h.infomask & IHT_LOCKED) && !(rh.ur_flags & IHUF_LOCKERS))
			report(cs, blk, off, "tuple is LOCKED, but undo record " UINT64_FORMAT " carries no lockers",
				   h.urec);

		ih_hdr_read(rh.ur_prev_hdr, &ph);
		if (rh.ur_type == IHU_LOCK)
		{
			if (!(h.infomask & IHT_LOCKED))
				report(cs, blk, off, "undo record " UINT64_FORMAT " is a lock, but the tuple is not LOCKED",
					   h.urec);
			if (ph.xid != h.xid)
				report(cs, blk, off, "lock record " UINT64_FORMAT " changes the version's creator from %u to %u",
					   h.urec, ph.xid, h.xid);
		}
		else
		{
			bool		isdel = (rh.ur_type == IHU_DELETE);
			bool		ismove = (rh.ur_type == IHU_MOVE);

			if (rh.ur_xid != h.xid)
				report(cs, blk, off, "version made by xid %u has undo record " UINT64_FORMAT " written by xid %u",
					   h.xid, h.urec, rh.ur_xid);
			if (isdel != ((h.infomask & IHT_DELETED) != 0) ||
				ismove != ((h.infomask & IHT_MOVED) != 0))
				report(cs, blk, off, "undo record " UINT64_FORMAT " of type %u does not match the version's flags %04X",
					   h.urec, rh.ur_type, h.infomask);
		}

		/* the version before, if any */
		switch (rh.ur_type)
		{
			case IHU_INSERT:
			case IHU_MULTI_INSERT:
				return;
			case IHU_UPDATE:
				{
					IHeapUpdateDelta d;

					if (core < sizeof(d))
					{
						report(cs, blk, off, "update record " UINT64_FORMAT " has no delta",
							   h.urec);
						return;
					}
					memcpy(&d, payload, sizeof(d));
					if (lenknown &&
						(uint32) d.prefix + d.suffix > curlen - ih_hdr_size_of(&h))
					{
						report(cs, blk, off, "update record " UINT64_FORMAT " keeps %u bytes of a version of %u bytes",
							   h.urec, d.prefix + d.suffix, curlen);
						return;
					}
					curlen = d.old_len;
					lenknown = true;
					break;
				}
			case IHU_DELETE:
			case IHU_MOVE:
				{
					/*
					 * The body of the version before: in the record (a
					 * compact header's, or one written before §21 D40), or
					 * on the page after the deletion's header, until the
					 * item is a stub.
					 */
					uint32		skip = (rh.ur_type == IHU_MOVE) ?
						sizeof(ItemPointerData) : 0;

					if (core > skip)
					{
						curlen = ih_hdr_size_of(&ph) + core - skip;
						lenknown = true;
					}
					else if (lenknown && onpage)
						curlen = curlen - ih_hdr_size_of(&h) + ih_hdr_size_of(&ph);
					else
						lenknown = false;
					break;
				}
			default:
				/* a lock: the same version */
				break;
		}
		if (rh.ur_type != IHU_LOCK)
			onpage = false;

		if (ph.urec == h.urec)
		{
			report(cs, blk, off, "undo record " UINT64_FORMAT " points to itself",
				   h.urec);
			return;
		}
		h = ph;
	}
}

/* is the version heading header h frozen?  Lock records on it aside. */
static bool
version_frozen(IHCheckState *cs, const IHeapHdr *h)
{
	UndoRecPtr	urec = h->urec;

	for (int attempt = 0; attempt < 2; attempt++)
	{
		if (attempt > 0)
			refresh_metapage(cs);
		if (urec < cs->discard)
			return true;
		if (h->infomask & IHT_LOCKED)
		{
			IHeapUndoRecHeader rh;
			char	   *payload;
			uint32		payload_len;
			const char *problem;

			if (!read_undo_record(cs, h->urec, &rh, &payload, &payload_len,
								  &problem))
				return problem == NULL; /* discarded meanwhile */
			if (rh.ur_type == IHU_LOCK)
			{
				IHeapHdr	ph;

				ih_hdr_read(rh.ur_prev_hdr, &ph);
				urec = ph.urec;
				if (urec < cs->discard)
					return true;
			}
		}
	}
	return false;
}

static void
check_tuple(IHCheckState *cs, BlockNumber blk, OffsetNumber off, Page page,
			ItemId lp, bool allvisible)
{
	char	   *tup = (char *) page + ItemIdGetOffset(lp);
	uint32		len = ItemIdGetLength(lp);
	uint32		lp_off = ItemIdGetOffset(lp);
	IHeapHdr	h;
	int			natts;
	bool		stub;
	bool		deleted;
	bool		walk = true;
	uint32		hoff;

	ih_hdr_read(tup, &h);
	natts = h.infomask2 & IHT_NATTS_MASK;
	stub = (h.infomask & IHT_STUB) != 0;
	deleted = (h.infomask & (IHT_DELETED | IHT_MOVED)) != 0;

	if (h.infomask & ~IHT_KNOWN_FLAGS)
		report(cs, blk, off, "tuple has unknown flags %04X",
			   h.infomask & ~IHT_KNOWN_FLAGS);
	if (h.infomask2 & ~IHT_NATTS_MASK)
		report(cs, blk, off, "tuple has unknown bits %04X in infomask2",
			   h.infomask2 & ~IHT_NATTS_MASK);
	if ((h.infomask & IHT_XID_HINTS) == IHT_XID_HINTS)
		report(cs, blk, off, "tuple's creator is hinted both committed and aborted");
	if ((h.infomask & IHT_DELETED) && (h.infomask & IHT_MOVED))
		report(cs, blk, off, "tuple is both DELETED and MOVED");
	if ((h.infomask & IHT_MOVED_PART) && !(h.infomask & IHT_DELETED))
		report(cs, blk, off, "tuple is MOVED_PART without being DELETED");
	if (stub && !deleted)
		report(cs, blk, off, "tuple is a stub, but neither DELETED nor MOVED");
	if (stub && len != IHEAP_TUPHDR_SIZE)
		report(cs, blk, off, "stub of %u bytes", len);
	if ((h.infomask & IHT_COMPACT) && (h.infomask & IHT_XID_FLAGS))
		report(cs, blk, off, "tuple has a compact header and flags %04X, which need an xid",
			   h.infomask & IHT_XID_FLAGS);
	if (len < ih_hdr_size_of(&h))
	{
		report(cs, blk, off, "tuple of %u bytes is shorter than its header", len);
		return;
	}

	if (natts > cs->desc->natts)
	{
		report(cs, blk, off, "tuple has %d attributes, the relation %d",
			   natts, cs->desc->natts);
		walk = false;
	}
	hoff = ih_hdr_size_of(&h) + ((h.infomask & IHT_HASNULL) ? BITMAPLEN(natts) : 0);
	if (h.hoff != hoff)
	{
		report(cs, blk, off, "tuple's header length is %u, expected %u",
			   h.hoff, hoff);
		walk = false;
	}
	else if (!stub && h.hoff > len)
	{
		report(cs, blk, off, "tuple's header length %u exceeds its length %u",
			   h.hoff, len);
		walk = false;
	}
	if (!iheap_tuple_aligned_at(tup, lp_off))
		report(cs, blk, off, "tuple at page offset %u is not aligned", lp_off);

	if (walk && !stub)
	{
		char	   *problem = attributes_problem(cs, tup, len, &h);

		if (problem)
			report(cs, blk, off, "%s", problem);
	}

	if (h.urec >= cs->discard)
		check_undo_chain(cs, blk, off, h, len, stub);

	/* an all-visible page holds frozen rows only (§10.4) */
	if (allvisible)
	{
		if (deleted || stub || (h.infomask & IHT_SPECULATIVE))
			report(cs, blk, off, "page is all-visible, but the tuple is %s",
				   (h.infomask & IHT_SPECULATIVE) ? "speculative" :
				   (h.infomask & IHT_MOVED) ? "moved" : "deleted");
		else if (!version_frozen(cs, &h))
			report(cs, blk, off, "page is all-visible, but the tuple is not frozen");
	}
}

typedef struct ItemSpan
{
	uint16		start;
	uint16		end;
	OffsetNumber off;
} ItemSpan;

static int
span_cmp(const void *a, const void *b)
{
	const ItemSpan *sa = (const ItemSpan *) a;
	const ItemSpan *sb = (const ItemSpan *) b;

	return (int) sa->start - (int) sb->start;
}

static void
check_data_page(IHCheckState *cs, BlockNumber blk, Page page, uint8 vmstatus)
{
	PageHeader	ph = (PageHeader) page;
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	bool		allvisible = (vmstatus & VISIBILITYMAP_ALL_VISIBLE) != 0;
	BlockMap   *map = NULL;
	ItemSpan   *spans;
	int			nspans = 0;

	if (cs->map != NULL && blk >= cs->map_start && blk < cs->map_end)
	{
		map = &cs->map[blk - cs->map_start];
		map->data = true;
	}

	if (maxoff > IHEAP_MAX_ITEMS)
	{
		report(cs, blk, InvalidOffsetNumber,
			   "data page has %u line pointers, at most %d are allowed",
			   maxoff, (int) IHEAP_MAX_ITEMS);
		maxoff = IHEAP_MAX_ITEMS;
	}
	if (map)
		map->maxoff = maxoff;

	if ((vmstatus & VISIBILITYMAP_ALL_FROZEN) && !allvisible)
		report(cs, blk, InvalidOffsetNumber,
			   "visibility map says all-frozen but not all-visible");
	if (allvisible && !PageIsAllVisible(page))
		report(cs, blk, InvalidOffsetNumber,
			   "visibility map says all-visible, the page does not");

	spans = palloc(Max(maxoff, 1) * sizeof(ItemSpan));
	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		lp = PageGetItemId(page, off);

		if (ItemIdIsRedirected(lp))
		{
			report(cs, blk, off, "redirect line pointer");
			continue;
		}
		if (!ItemIdIsUsed(lp))
		{
			if (ItemIdHasStorage(lp))
				report(cs, blk, off, "unused line pointer has storage");
			continue;
		}
		if (map)
			map->used[(off - 1) / 8] |= 1 << ((off - 1) % 8);
		if (ItemIdIsDead(lp))
		{
			if (ItemIdHasStorage(lp))
				report(cs, blk, off, "dead line pointer has storage");
			if (allvisible)
				report(cs, blk, off, "page is all-visible, but has a dead line pointer");
			continue;
		}

		if (ItemIdGetOffset(lp) < ph->pd_upper ||
			ItemIdGetOffset(lp) + ItemIdGetLength(lp) > ph->pd_special ||
			ItemIdGetLength(lp) < IHEAP_COMPACT_HDRSZ)
		{
			report(cs, blk, off, "line pointer to %u of %u bytes is outside the tuple space %u to %u",
				   ItemIdGetOffset(lp), ItemIdGetLength(lp), ph->pd_upper,
				   ph->pd_special);
			continue;
		}
		spans[nspans].start = ItemIdGetOffset(lp);
		spans[nspans].end = ItemIdGetOffset(lp) + ItemIdGetLength(lp);
		spans[nspans].off = off;
		nspans++;

		check_tuple(cs, blk, off, page, lp, allvisible);
	}

	qsort(spans, nspans, sizeof(ItemSpan), span_cmp);
	for (int i = 1; i < nspans; i++)
	{
		if (spans[i].start < spans[i - 1].end)
			report(cs, blk, spans[i].off,
				   "tuple overlaps the tuple of line pointer %u",
				   spans[i - 1].off);
	}
}

/* ------------------------------------------------------------------------
 * Pages
 * ------------------------------------------------------------------------
 */
static bool
page_header_ok(IHCheckState *cs, BlockNumber blk, Page page)
{
	PageHeader	ph = (PageHeader) page;

	if (ph->pd_lower < SizeOfPageHeaderData || ph->pd_lower > ph->pd_upper ||
		ph->pd_upper > ph->pd_special ||
		ph->pd_special != BLCKSZ - IHEAP_SPECIAL_SIZE ||
		PageGetPageSize(page) != BLCKSZ)
	{
		report(cs, blk, InvalidOffsetNumber,
			   "page header is corrupt (lower %u, upper %u, special %u)",
			   ph->pd_lower, ph->pd_upper, ph->pd_special);
		return false;
	}
	return true;
}

static const char *
type_name(Page page)
{
	switch (IHeapPageGetType(page))
	{
		case IHEAP_PAGE_META:
			return "a metapage";
		case IHEAP_PAGE_DATA:
			return "a data page";
		case IHEAP_PAGE_UNDO:
			return "an undo page";
		default:
			return "a page of unknown type";
	}
}

static void
check_block(IHCheckState *cs, BlockNumber blk)
{
	Buffer		buf;
	Buffer		vmbuf = InvalidBuffer;
	uint8		vmstatus = 0;
	char	   *page = palloc(BLCKSZ);
	IHeapExtent *ext;
	uint16		type;

	buf = ReadBufferExtended(cs->rel, MAIN_FORKNUM, blk, RBM_NORMAL,
							 cs->bstrategy);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	/* the bit is cleared and set with the page locked exclusively */
	if (IHeapPageIsData(BufferGetPage(buf)))
		vmstatus = visibilitymap_get_status(cs->rel, blk, &vmbuf);
	memcpy(page, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	if (BufferIsValid(vmbuf))
		ReleaseBuffer(vmbuf);

	if (PageIsNew(page))
		return;
	if (!page_header_ok(cs, blk, page))
		return;
	type = IHeapPageGetType(page);

	ext = block_extent(cs, blk);
	if (ext != NULL && type == IHEAP_PAGE_DATA)
	{
		/* the extent may have been dropped meanwhile */
		refresh_metapage(cs);
		ext = block_extent(cs, blk);
	}

	if (ext == NULL)
	{
		/* data, or undo of an extent dropped since */
		if (type == IHEAP_PAGE_DATA)
			check_data_page(cs, blk, page, vmstatus);
		else if (type != IHEAP_PAGE_UNDO)
			report(cs, blk, InvalidOffsetNumber, "block outside undo extents is %s",
				   type_name(page));
		return;
	}

	if (type != IHEAP_PAGE_UNDO)
	{
		report(cs, blk, InvalidOffsetNumber, "block of an undo extent is %s",
			   type_name(page));
		return;
	}
	if (ext->ie_lstart != IHEAP_EXTENT_FREE)
	{
		uint64		lpage = ext->ie_lstart + (blk - ext->ie_pstart);

		if (lpage < UndoRecPtrGetLPage(cs->discard) || lpage >= cs->next_lpage)
			return;				/* discarded, or not written yet */
		if (IHeapUndoPageGetHeader(page)->uph_lpage != lpage)
		{
			refresh_metapage(cs);
			if (lpage >= UndoRecPtrGetLPage(cs->discard))
				report(cs, blk, InvalidOffsetNumber,
					   "undo page " UINT64_FORMAT " is marked as undo page " UINT64_FORMAT,
					   lpage, IHeapUndoPageGetHeader(page)->uph_lpage);
			return;
		}
		check_undo_page(cs, blk, page, lpage);
	}
}

/* ------------------------------------------------------------------------
 * Indexes
 * ------------------------------------------------------------------------
 */
typedef struct RecheckArg
{
	IHCheckState *cs;
	ItemPointerData tid;
	const char *problem;
} RecheckArg;

/* the line pointer an index entry points to, with the leaf page locked */
static bool
recheck_entry(void *arg)
{
	RecheckArg *a = (RecheckArg *) arg;
	BlockNumber blk = ItemPointerGetBlockNumber(&a->tid);
	OffsetNumber off = ItemPointerGetOffsetNumber(&a->tid);
	Buffer		buf;
	Page		page;

	a->problem = NULL;
	if (blk >= RelationGetNumberOfBlocks(a->cs->rel))
	{
		a->problem = "beyond the end of the table";
		return false;
	}
	buf = ReadBufferExtended(a->cs->rel, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	if (!IHeapPageIsData(page))
		a->problem = "to a page that holds no rows";
	else if (off < FirstOffsetNumber || off > PageGetMaxOffsetNumber(page) ||
			 !ItemIdIsUsed(PageGetItemId(page, off)))
		a->problem = "to an unused line pointer";
	UnlockReleaseBuffer(buf);
	return a->problem == NULL;
}

/* the entries of index pointing to the blocks of the current range */
static void
check_index_range(IHCheckState *cs, Relation index)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	ItemPointerData *tids = palloc(dmbtree_max_leaf_tids() * sizeof(ItemPointerData));
	bool		last = (cs->map_end >= cs->nblocks);

	for (BlockNumber iblk = 1; iblk < nblocks; iblk++)
	{
		int			n;

		CHECK_FOR_INTERRUPTS();
		n = dmbtree_leaf_heap_tids(index, iblk, cs->bstrategy, tids);
		for (int i = 0; i < n; i++)
		{
			BlockNumber blk = ItemPointerGetBlockNumber(&tids[i]);
			OffsetNumber off = ItemPointerGetOffsetNumber(&tids[i]);
			BlockMap   *m;
			RecheckArg	a;

			if (blk < cs->map_start || (blk >= cs->map_end && !last))
				continue;
			if (blk < cs->map_end)
			{
				m = &cs->map[blk - cs->map_start];
				if (m->data && off >= FirstOffsetNumber && off <= m->maxoff &&
					(m->used[(off - 1) / 8] & (1 << ((off - 1) % 8))))
					continue;
			}
			/* not in use when the table was read: look again */
			a.cs = cs;
			a.tid = tids[i];
			a.problem = NULL;
			if (!dmbtree_leaf_recheck(index, iblk, &tids[i], recheck_entry, &a))
				report(cs, blk, off, "index \"%s\" (block %u) points %s",
					   RelationGetRelationName(index), iblk, a.problem);
		}
	}
	pfree(tids);
}

/* btree and dmbtree indexes of the relation, opened */
static List *
open_indexes(Relation rel)
{
	List	   *indexes = NIL;
	List	   *oids = RelationGetIndexList(rel);
	ListCell   *lc;

	foreach(lc, oids)
	{
		Relation	index = index_open(lfirst_oid(lc), AccessShareLock);

		if (index->rd_rel->relam == BTREE_AM_OID || dmbtree_is_dmbtree(index))
			indexes = lappend(indexes, index);
		else
			index_close(index, AccessShareLock);
	}
	list_free(oids);
	return indexes;
}

/* ------------------------------------------------------------------------
 * iheap_check(rel regclass, check_indexes boolean)
 *	-> setof (blkno int8, offnum int4, msg text)
 * ------------------------------------------------------------------------
 */
Datum
iheap_check(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	bool		check_indexes = PG_GETARG_BOOL(1);
	IHCheckState cs;
	BlockNumber chunk;
	ListCell   *lc;

	InitMaterializedSRF(fcinfo, 0);

	memset(&cs, 0, sizeof(cs));
	cs.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	cs.cxt = CurrentMemoryContext;
	cs.rel = iheap_open_relation(relid, AccessShareLock);
	if (RELATION_IS_OTHER_TEMP(cs.rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot check temporary tables of other sessions")));
	if (cs.rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
		RecoveryInProgress())
	{
		ereport(NOTICE,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot check unlogged relation \"%s\" during recovery, skipping",
						RelationGetRelationName(cs.rel))));
		relation_close(cs.rel, AccessShareLock);
		return (Datum) 0;
	}

	cs.pagecxt = AllocSetContextCreate(CurrentMemoryContext, "iheap_check page",
									   ALLOCSET_DEFAULT_SIZES);
	cs.bstrategy = GetAccessStrategy(BAS_BULKREAD);
	cs.desc = RelationGetDescr(cs.rel);
	cs.toastrelid = cs.rel->rd_rel->reltoastrelid;
	cs.relfrozenxid = cs.rel->rd_rel->relfrozenxid;
	cs.nextxid = ReadNextTransactionId();
	cs.nblocks = RelationGetNumberOfBlocks(cs.rel);

	if (cs.nblocks == 0)
		report(&cs, IHEAP_METAPAGE_BLKNO, InvalidOffsetNumber,
			   "relation has no metapage");
	else if (read_metapage(&cs, true))
	{
		if (check_indexes)
			cs.indexes = open_indexes(cs.rel);

		/* with indexes, the table is checked range by range (BlockMap) */
		chunk = cs.nblocks;
		if (cs.indexes != NIL)
			chunk = (BlockNumber) Max(Min((Size) maintenance_work_mem * 1024,
										  MaxAllocSize) / sizeof(BlockMap), 1);

		for (BlockNumber start = 0; start < cs.nblocks;
			 start = (cs.nblocks - start > chunk) ? start + chunk : cs.nblocks)
		{
			BlockNumber end = (cs.nblocks - start > chunk) ? start + chunk : cs.nblocks;

			if (cs.indexes != NIL)
			{
				cs.map_start = start;
				cs.map_end = end;
				cs.map = MemoryContextAllocZero(cs.cxt,
												(end - start) * sizeof(BlockMap));
			}
			for (BlockNumber blk = Max(start, 1); blk < end; blk++)
			{
				CHECK_FOR_INTERRUPTS();
				MemoryContextReset(cs.pagecxt);
				MemoryContextSwitchTo(cs.pagecxt);
				check_block(&cs, blk);
				MemoryContextSwitchTo(cs.cxt);
			}
			if (cs.indexes != NIL)
			{
				foreach(lc, cs.indexes)
				{
					MemoryContextReset(cs.pagecxt);
					MemoryContextSwitchTo(cs.pagecxt);
					check_index_range(&cs, (Relation) lfirst(lc));
					MemoryContextSwitchTo(cs.cxt);
				}
				pfree(cs.map);
				cs.map = NULL;
			}
		}

		foreach(lc, cs.indexes)
			index_close((Relation) lfirst(lc), AccessShareLock);
	}

	FreeAccessStrategy(cs.bstrategy);
	MemoryContextDelete(cs.pagecxt);
	relation_close(cs.rel, AccessShareLock);
	return (Datum) 0;
}
