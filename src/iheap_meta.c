/*-------------------------------------------------------------------------
 *
 * iheap_meta.c
 *	  Metapage, undo extents and the relcache-attached metapage cache.
 *
 * The metapage (block 0) records the relation's undo stream: the discard
 * pointer, the next logical undo page to allocate and the list of extents
 * (contiguous runs of physical pages) holding live or free undo pages.
 *
 * Backends cache the live part of it in rel->rd_amcache, like nbtree caches
 * its metapage.  A stale cache is safe: a lower discard pointer only makes
 * fewer tuples look frozen, and undo page headers carry their logical page
 * number, so a page reused for something else is recognized (§21 D18).
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "iheap.h"

void
iheap_init_metapage(Page page)
{
	IHeapMetaPageData *meta;

	PageInit(page, BLCKSZ, IHEAP_SPECIAL_SIZE);
	IHeapPageGetOpaque(page)->ihp_type = IHEAP_PAGE_META;

	meta = IHeapPageGetMeta(page);
	meta->im_magic = IHEAP_META_MAGIC;
	meta->im_version = IHEAP_META_VERSION;
	/* logical page 0 is never used, so undo pointer 0 is always frozen */
	meta->im_discard = MakeUndoRecPtr(1, 0);
	meta->im_next_lpage = 1;
	meta->im_nextents = 0;
	meta->im_flags = 0;
	meta->im_undo_peak = 0;
	meta->im_pad = 0;

	((PageHeader) page)->pd_lower =
		((char *) meta + offsetof(IHeapMetaPageData, im_extents)) - (char *) page;
}

void
iheap_init_data_page(Page page)
{
	PageInit(page, BLCKSZ, IHEAP_SPECIAL_SIZE);
	IHeapPageGetOpaque(page)->ihp_type = IHEAP_PAGE_DATA;
}

/* keep pd_lower at the end of the extents array (FPI hole compression) */
void
iheap_meta_set_lower(Page page)
{
	IHeapMetaPageData *meta = IHeapPageGetMeta(page);

	((PageHeader) page)->pd_lower =
		((char *) &meta->im_extents[meta->im_nextents]) - (char *) page;
}

/* bytes of the metapage that describe its state (used as WAL block data) */
Size
iheap_meta_image_size(Page page)
{
	IHeapMetaPageData *meta = IHeapPageGetMeta(page);

	return offsetof(IHeapMetaPageData, im_extents) +
		meta->im_nextents * sizeof(IHeapExtent);
}

static void
iheap_meta_validate(Relation rel, Page page)
{
	IHeapMetaPageData *meta;

	if (IHeapPageGetType(page) != IHEAP_PAGE_META)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block 0 of relation \"%s\" is not an iheap metapage",
						RelationGetRelationName(rel))));
	meta = IHeapPageGetMeta(page);
	if (meta->im_magic != IHEAP_META_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("iheap metapage of relation \"%s\" has bad magic %08X",
						RelationGetRelationName(rel), meta->im_magic)));
	if (meta->im_version != IHEAP_META_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("iheap relation \"%s\" has on-disk format version %u, expected %u",
						RelationGetRelationName(rel), meta->im_version,
						IHEAP_META_VERSION)));
}

/*
 * Rebuild the relcache-attached cache from a metapage the caller holds
 * locked.  Must not be called inside a critical section.
 */
void
iheap_relcache_from_meta(Relation rel, Page metapage)
{
	IHeapMetaPageData *meta = IHeapPageGetMeta(metapage);
	IHeapRelCache *rc;
	int			nlive = 0;

	for (uint32 i = 0; i < meta->im_nextents; i++)
		if (meta->im_extents[i].ie_lstart != IHEAP_EXTENT_FREE)
			nlive++;

	rc = MemoryContextAllocZero(CacheMemoryContext,
								offsetof(IHeapRelCache, rc_live) +
								(nlive + meta->im_nextents + 1) * sizeof(IHeapExtent));
	rc->rc_magic = IHEAP_RELCACHE_MAGIC;
	rc->rc_discard = meta->im_discard;
	rc->rc_next_lpage = meta->im_next_lpage;
	rc->rc_refreshed = GetCurrentTimestamp();
	rc->rc_nlive = 0;
	for (uint32 i = 0; i < meta->im_nextents; i++)
	{
		if (meta->im_extents[i].ie_lstart != IHEAP_EXTENT_FREE)
			rc->rc_live[rc->rc_nlive++] = meta->im_extents[i];
	}

	/* every extent by physical start, for scans that skip undo */
	rc->rc_all = &rc->rc_live[nlive];
	rc->rc_nall = 0;
	rc->rc_extent_pages = 0;
	for (uint32 i = 0; i < meta->im_nextents; i++)
	{
		IHeapExtent tmp = meta->im_extents[i];
		int			j = rc->rc_nall - 1;

		while (j >= 0 && rc->rc_all[j].ie_pstart > tmp.ie_pstart)
		{
			rc->rc_all[j + 1] = rc->rc_all[j];
			j--;
		}
		rc->rc_all[j + 1] = tmp;
		rc->rc_nall++;
		rc->rc_extent_pages += tmp.ie_npages;
	}

	/* live extents are kept sorted by logical start */
	for (int i = 1; i < rc->rc_nlive; i++)
	{
		IHeapExtent tmp = rc->rc_live[i];
		int			j = i - 1;

		while (j >= 0 && rc->rc_live[j].ie_lstart > tmp.ie_lstart)
		{
			rc->rc_live[j + 1] = rc->rc_live[j];
			j--;
		}
		rc->rc_live[j + 1] = tmp;
	}

	if (rel->rd_amcache)
		pfree(rel->rd_amcache);
	rel->rd_amcache = rc;
}

/*
 * Return the cached metapage state, reading the metapage if there is no
 * cache, if "refresh" is requested, or if the cache predates the current
 * statement.  The result must be used at once: relcache invalidation frees
 * it.
 */
IHeapRelCache *
iheap_relcache(Relation rel, bool refresh)
{
	IHeapRelCache *rc = (IHeapRelCache *) rel->rd_amcache;
	Buffer		buf;
	Page		page;

	if (rc != NULL && rc->rc_magic == IHEAP_RELCACHE_MAGIC && !refresh &&
		(!IsTransactionState() ||
		 rc->rc_refreshed >= GetCurrentStatementStartTimestamp()))
		return rc;

	buf = ReadBufferExtended(rel, MAIN_FORKNUM, IHEAP_METAPAGE_BLKNO,
							 RBM_NORMAL, NULL);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	iheap_meta_validate(rel, page);
	iheap_relcache_from_meta(rel, page);
	UnlockReleaseBuffer(buf);

	return (IHeapRelCache *) rel->rd_amcache;
}

void
iheap_relcache_invalidate(Relation rel)
{
	if (rel->rd_amcache)
	{
		pfree(rel->rd_amcache);
		rel->rd_amcache = NULL;
	}
}

/*
 * Block ranges that hold no data (the metapage and every undo extent), in
 * physical order, as a palloc'd array.  Read from the metapage itself (the
 * cache can be as old as the top-level statement, older than the caller's
 * snapshot): extents only become data pages by being dropped from the
 * metapage, and data pages never become undo, so a page skipped by a scan
 * that took this after its snapshot holds nothing that snapshot can see.
 */
int
iheap_nondata_ranges(Relation rel, IHeapExtent **ranges)
{
	IHeapRelCache *rc = iheap_relcache(rel, true);
	IHeapExtent *r = palloc((rc->rc_nall + 1) * sizeof(IHeapExtent));

	r[0].ie_lstart = IHEAP_EXTENT_FREE;
	r[0].ie_pstart = IHEAP_METAPAGE_BLKNO;
	r[0].ie_npages = 1;
	memcpy(&r[1], rc->rc_all, rc->rc_nall * sizeof(IHeapExtent));
	*ranges = r;
	return rc->rc_nall + 1;
}

/* pages held by undo extents, live or free */
uint32
iheap_extent_pages(Relation rel)
{
	return iheap_relcache(rel, false)->rc_extent_pages;
}

UndoRecPtr
iheap_discard_ptr(Relation rel)
{
	return iheap_relcache(rel, false)->rc_discard;
}

static bool
iheap_lookup_extent(IHeapRelCache *rc, uint64 lpage, BlockNumber *blk)
{
	int			lo = 0;
	int			hi = rc->rc_nlive - 1;

	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;
		IHeapExtent *e = &rc->rc_live[mid];

		if (lpage < e->ie_lstart)
			hi = mid - 1;
		else if (lpage >= e->ie_lstart + e->ie_npages)
			lo = mid + 1;
		else
		{
			*blk = e->ie_pstart + (BlockNumber) (lpage - e->ie_lstart);
			return true;
		}
	}
	return false;
}

/*
 * Map a logical undo page to its physical block.  *found is false when the
 * page is not part of a live extent (it has been discarded).
 */
BlockNumber
iheap_lpage_to_block(Relation rel, uint64 lpage, bool *found)
{
	IHeapRelCache *rc = iheap_relcache(rel, false);
	BlockNumber blk;

	if (iheap_lookup_extent(rc, lpage, &blk))
	{
		*found = true;
		return blk;
	}

	/* maybe a newer extent: refresh once */
	rc = iheap_relcache(rel, true);
	if (iheap_lookup_extent(rc, lpage, &blk))
	{
		*found = true;
		return blk;
	}

	*found = false;
	return InvalidBlockNumber;
}

/* ------------------------------------------------------------------------
 * Metapage manipulation (caller holds an exclusive lock on the metapage)
 * ------------------------------------------------------------------------
 */

/* find the live extent that contains lpage, or NULL */
IHeapExtent *
iheap_meta_find_extent(IHeapMetaPageData *meta, uint64 lpage)
{
	for (uint32 i = 0; i < meta->im_nextents; i++)
	{
		IHeapExtent *e = &meta->im_extents[i];

		if (e->ie_lstart != IHEAP_EXTENT_FREE &&
			lpage >= e->ie_lstart && lpage < e->ie_lstart + e->ie_npages)
			return e;
	}
	return NULL;
}

/* number of live undo pages between the discard point and next_lpage */
uint64
iheap_meta_live_pages(IHeapMetaPageData *meta)
{
	uint64		first = UndoRecPtrGetLPage(meta->im_discard);

	if (meta->im_next_lpage <= first)
		return 0;
	return meta->im_next_lpage - first;
}

/*
 * Free extents entirely below the discard point, after a discard that passed
 * "passed" logical pages.  Free extents are kept for reuse by undo while the
 * pages of live and kept extents stay within im_undo_peak, a high-water mark
 * of the pages in live extents that decays by 1/64 of the undo passed, and
 * at least IHEAP_FREE_EXTENT_RESERVE of them always.  Undo comes in bursts (a
 * long query holds back discard); without the reserve each burst extended
 * the relation anew while the space of the last one sat in the FSM, where
 * only data could use it.  The others are dropped from the list and their
 * blocks returned in *released, for the caller to hand to the FSM after the
 * WAL record is written.
 */
int
iheap_meta_free_extents(IHeapMetaPageData *meta, IHeapExtent *released,
						int maxreleased, uint64 passed)
{
	uint64		discard_lpage = UndoRecPtrGetLPage(meta->im_discard);
	uint64		live = 0;
	uint64		budget;
	uint64		kept = 0;
	int			nkept = 0;
	int			nreleased = 0;
	uint32		j;

	for (uint32 i = 0; i < meta->im_nextents; i++)
	{
		IHeapExtent *e = &meta->im_extents[i];

		if (e->ie_lstart != IHEAP_EXTENT_FREE &&
			e->ie_lstart + e->ie_npages <= discard_lpage)
			e->ie_lstart = IHEAP_EXTENT_FREE;
		if (e->ie_lstart != IHEAP_EXTENT_FREE)
			live += e->ie_npages;
	}

	if (live >= meta->im_undo_peak)
		meta->im_undo_peak = (uint32) Min(live, PG_UINT32_MAX);
	else
		meta->im_undo_peak -= (uint32) Min(meta->im_undo_peak - live,
										   (passed + 63) / 64);
	budget = meta->im_undo_peak;

	j = 0;
	for (uint32 i = 0; i < meta->im_nextents; i++)
	{
		IHeapExtent *e = &meta->im_extents[i];

		if (e->ie_lstart == IHEAP_EXTENT_FREE)
		{
			if (nkept < IHEAP_FREE_EXTENT_RESERVE ||
				live + kept + e->ie_npages <= budget ||
				nreleased >= maxreleased)
			{
				kept += e->ie_npages;
				nkept++;
			}
			else
			{
				released[nreleased++] = *e;
				continue;
			}
		}
		meta->im_extents[j++] = *e;
	}
	meta->im_nextents = j;

	return nreleased;
}

/*
 * Pick the size of a new extent: roughly the current amount of live undo,
 * so that the extent list grows geometrically, within [MIN, MAX].
 */
uint32
iheap_meta_next_extent_size(IHeapMetaPageData *meta)
{
	uint64		live = iheap_meta_live_pages(meta);
	uint32		npages = IHEAP_EXTENT_MIN_PAGES;

	while (npages < live && npages < IHEAP_EXTENT_MAX_PAGES)
		npages *= 2;
	return Min(npages, IHEAP_EXTENT_MAX_PAGES);
}

/* is block "blk" inside any extent (live or free)? */
bool
iheap_meta_block_in_extent(IHeapMetaPageData *meta, BlockNumber blk)
{
	for (uint32 i = 0; i < meta->im_nextents; i++)
	{
		IHeapExtent *e = &meta->im_extents[i];

		if (blk >= e->ie_pstart && blk < e->ie_pstart + e->ie_npages)
			return true;
	}
	return false;
}
