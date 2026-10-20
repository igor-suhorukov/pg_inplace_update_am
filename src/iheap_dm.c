/*-------------------------------------------------------------------------
 *
 * iheap_dm.c
 *	  iheap's side of dmbtree: in-place updates of indexed columns
 *	  (DESIGN.md §7.4, §11.5, §11.6, §21 D22-D27).
 *
 * An UPDATE that changes a key column of a dmbtree index keeps the row at
 * its TID.  iheap then maintains such indexes itself: it inserts an entry
 * for the new key and delete-marks the entry for the old one (the executor
 * can only add entries to every index, TU_All, or to none).  The new entry
 * is marked too, since the update may still abort and rollback never
 * touches indexes: a marked entry may disagree with its row, so whoever
 * reaches a row through one checks the row's key against it.  An unmarked
 * entry agrees with the newest committed version of its row.
 *
 * This file has:
 *	- the per-relation cache of the indexes that can take in-place updates
 *	  of their key columns, and of the columns whose change moves the row;
 *	- the index maintenance after an in-place update;
 *	- the key check (iheap_dm_matches), used by index fetches (iheap_scan.c)
 *	  and by the functions dmbtree calls (dmbtree_api.h): the check for
 *	  index-only scans, the probe for uniqueness checks, the fate of marked
 *	  entries for cleanup, and the check of entries against their rows for
 *	  dmbtree_check().
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/relation.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_index.h"
#include "executor/tuptable.h"
#include "nodes/bitmapset.h"
#include "optimizer/optimizer.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "utils/datum.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "iheap.h"
#include "dmbtree_api.h"

/* ------------------------------------------------------------------------
 * Which indexes can take in-place updates of their key columns
 * ------------------------------------------------------------------------
 */
static HTAB *dm_infos = NULL;
static HTAB *dm_owners = NULL;	/* index OID -> table, for the dm[] indexes */
static uint64 dm_invalidations = 0;	/* callback calls, to detect races */
static int	dm_nstale = 0;		/* entries gone stale since the last sweep */

typedef struct DmOwner
{
	Oid			indexoid;		/* hash key */
	Oid			relid;
} DmOwner;

static void
dm_mark_stale(IHeapDmInfo *info)
{
	info->valid = false;
	dm_nstale++;
}

/*
 * A relcache invalidation of the table (CREATE/DROP INDEX, changes of an
 * index's validity, REINDEX) or of one of its indexes.  It can arrive
 * whenever a lock is taken, also while an update holds on to the entry, so
 * the entry is only marked stale here; the next iheap_dm_info() call
 * rebuilds it, and the end of the transaction frees it if none did (the
 * table may be gone).
 */
static void
dm_relcache_callback(Datum arg, Oid relid)
{
	IHeapDmInfo *info;
	DmOwner    *owner;

	dm_invalidations++;
	if (dm_infos == NULL)
		return;

	if (!OidIsValid(relid))
	{
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, dm_infos);
		while ((info = hash_seq_search(&status)) != NULL)
			dm_mark_stale(info);
		return;
	}

	info = hash_search(dm_infos, &relid, HASH_FIND, NULL);
	if (info != NULL && info->valid)
		dm_mark_stale(info);
	owner = hash_search(dm_owners, &relid, HASH_FIND, NULL);
	if (owner != NULL)
	{
		info = hash_search(dm_infos, &owner->relid, HASH_FIND, NULL);
		if (info != NULL && info->valid)
			dm_mark_stale(info);
	}
}

/* forget info's contents, and the owner entries of its indexes */
static void
dm_free_contents(IHeapDmInfo *info)
{
	if (info->cxt == NULL)
		return;
	for (int i = 0; i < info->ndm; i++)
	{
		DmOwner    *owner = hash_search(dm_owners, &info->dm[i].indexoid,
										HASH_FIND, NULL);

		if (owner != NULL && owner->relid == info->relid)
			hash_search(dm_owners, &info->dm[i].indexoid, HASH_REMOVE, NULL);
	}
	MemoryContextDelete(info->cxt);
	info->cxt = NULL;
	info->ndm = 0;
}

/* at the end of a transaction nobody holds an entry: free the stale ones */
static void
dm_xact_callback(XactEvent event, void *arg)
{
	HASH_SEQ_STATUS status;
	IHeapDmInfo *info;

	if (dm_nstale == 0 ||
		(event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT &&
		 event != XACT_EVENT_PREPARE))
		return;

	hash_seq_init(&status, dm_infos);
	while ((info = hash_seq_search(&status)) != NULL)
	{
		if (info->valid)
			continue;
		dm_free_contents(info);
		hash_search(dm_infos, &info->relid, HASH_REMOVE, NULL);
	}
	dm_nstale = 0;
}

void
iheap_dm_init(void)
{
	CacheRegisterRelcacheCallback(dm_relcache_callback, (Datum) 0);
	RegisterXactCallback(dm_xact_callback, NULL);
}

static void
add_attnum(Bitmapset **set, AttrNumber attnum)
{
	*set = bms_add_member(*set, attnum - FirstLowInvalidHeapAttributeNumber);
}

/*
 * The cached facts about rel's indexes.  An index takes in-place updates of
 * its key columns when it is a dmbtree index that can carry delete-marks
 * (dmbtree_dm_capable), is valid and ready, and its uniqueness is checked
 * immediately: a deferred check or an exclusion constraint is the
 * executor's business, which needs entries for a new TID (rule 2 of §7.4).
 * Every column of any other index (summarizing ones aside, which
 * TU_Summarizing handles) forces a move, as do INCLUDE columns: an
 * index-only scan returns them from the entry, and an entry stands for
 * every version of its row with that key.
 *
 * The result stays valid until the next call for the same relation.
 */
IHeapDmInfo *
iheap_dm_info(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	IHeapDmInfo *info;
	bool		found;
	List	   *indexoids;
	ListCell   *lc;
	MemoryContext oldcxt;
	int			maxdm;
	uint64		invalidations;

	if (dm_infos == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(IHeapDmInfo);
		ctl.hcxt = CacheMemoryContext;
		dm_infos = hash_create("iheap dmbtree indexes", 64, &ctl,
							   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		ctl.entrysize = sizeof(DmOwner);
		dm_owners = hash_create("iheap dmbtree index owners", 64, &ctl,
								HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	info = hash_search(dm_infos, &relid, HASH_ENTER, &found);
	if (!found)
	{
		info->cxt = NULL;
		info->ndm = 0;
		dm_mark_stale(info);	/* swept if never built */
	}
	if (info->valid)
		return info;

restart:
	dm_free_contents(info);
	invalidations = dm_invalidations;

	indexoids = RelationGetIndexList(rel);

	info->cxt = AllocSetContextCreate(CacheMemoryContext, "iheap dmbtree index info",
									  ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(info->cxt);
	info->blocking = NULL;
	info->dmattrs = NULL;
	info->ndm = 0;
	maxdm = Max(list_length(indexoids), 1);
	info->dm = palloc0(maxdm * sizeof(IHeapDmIndex));
	MemoryContextSwitchTo(oldcxt);

	foreach(lc, indexoids)
	{
		Relation	irel = index_open(lfirst_oid(lc), AccessShareLock);
		Form_pg_index idx = irel->rd_index;
		int			natts = IndexRelationGetNumberOfAttributes(irel);
		int			nkeyatts = IndexRelationGetNumberOfKeyAttributes(irel);
		Bitmapset  *attrs = NULL;

		if (irel->rd_indam->amsummarizing)
		{
			index_close(irel, AccessShareLock);
			continue;
		}

		oldcxt = MemoryContextSwitchTo(info->cxt);
		if (idx->indisvalid && idx->indisready && idx->indislive &&
			!idx->indisexclusion &&
			(!idx->indisunique || idx->indimmediate) &&
			dmbtree_dm_capable(irel))
		{
			IHeapDmIndex *d = &info->dm[info->ndm++];

			d->indexoid = RelationGetRelid(irel);
			d->natts = natts;
			d->unique = idx->indisunique;
			d->keyattrs = NULL;
			for (int i = 0; i < natts; i++)
			{
				d->attnums[i] = idx->indkey.values[i];
				if (i < nkeyatts)
					add_attnum(&d->keyattrs, d->attnums[i]);
				else
					add_attnum(&info->blocking, d->attnums[i]);
			}
			info->dmattrs = bms_add_members(info->dmattrs, d->keyattrs);
		}
		else
		{
			for (int i = 0; i < natts; i++)
			{
				if (idx->indkey.values[i] != 0)
					add_attnum(&attrs, idx->indkey.values[i]);
			}
			pull_varattnos((Node *) RelationGetIndexExpressions(irel), 1, &attrs);
			pull_varattnos((Node *) RelationGetIndexPredicate(irel), 1, &attrs);
			info->blocking = bms_add_members(info->blocking, attrs);
		}
		MemoryContextSwitchTo(oldcxt);

		index_close(irel, AccessShareLock);
	}
	list_free(indexoids);

	/* opening the indexes may have processed invalidations: be sure */
	if (dm_invalidations != invalidations)
		goto restart;

	for (int i = 0; i < info->ndm; i++)
	{
		DmOwner    *owner = hash_search(dm_owners, &info->dm[i].indexoid,
										HASH_ENTER, NULL);

		owner->relid = relid;
	}
	info->valid = true;
	return info;
}

/* ------------------------------------------------------------------------
 * Index maintenance after an in-place update
 * ------------------------------------------------------------------------
 */

/*
 * The in-place update of row tid changed the columns "modified": for every
 * index whose key it changed, insert the entry for the new key and
 * delete-mark the entry for the old one (§11.6).  The table change is done
 * and its page lock released: index pages are never locked while a table
 * page is (nbtree's uniqueness checks lock them the other way round).
 * Other writers wait on our row until we finish; readers of our
 * transaction see the row only after this command, when both entries are
 * there.
 *
 * The new entry goes first: if it violates uniqueness, the transaction
 * aborts before the old one is marked (either order is safe).
 */
void
iheap_dm_update_indexes(Relation rel, ItemPointer tid,
						const Bitmapset *modified,
						const Datum *oldvalues, const bool *oldisnull,
						const Datum *newvalues, const bool *newisnull)
{
	IHeapDmInfo *info = iheap_dm_info(rel);

	for (int i = 0; i < info->ndm; i++)
	{
		IHeapDmIndex *d = &info->dm[i];
		Relation	irel;
		Datum		oldkey[INDEX_MAX_KEYS];
		bool		oldnull[INDEX_MAX_KEYS];
		Datum		newkey[INDEX_MAX_KEYS];
		bool		newnull[INDEX_MAX_KEYS];

		if (!bms_overlap(d->keyattrs, modified))
			continue;

		for (int a = 0; a < d->natts; a++)
		{
			int			col = d->attnums[a] - 1;

			oldkey[a] = oldvalues[col];
			oldnull[a] = oldisnull[col];
			newkey[a] = newvalues[col];
			newnull[a] = newisnull[col];
		}

		irel = index_open(d->indexoid, RowExclusiveLock);
		dmbtree_inplace_insert(irel, rel, newkey, newnull, tid, d->unique);
		dmbtree_inplace_mark(irel, rel, oldkey, oldnull, tid);
		index_close(irel, NoLock);
	}
}

/* ------------------------------------------------------------------------
 * The key check
 * ------------------------------------------------------------------------
 */

/*
 * Does the row version "tuple" have the key of entry dm?  Returns 1 or 0,
 * or -1 if it cannot tell without fetching a TOASTed value, which only a
 * caller with a snapshot may do (allow_external).
 *
 * The index's opclasses have "equality implies image equality"
 * (dmbtree_dm_capable), so keys are compared by image, which also copes
 * with values compressed differently in the index and the table.
 */
int
iheap_dm_matches(Relation rel, const IHeapDmEntry *dm, const char *tuple,
				 bool allow_external)
{
	Relation	index = dm->index;
	TupleDesc	tdesc = RelationGetDescr(rel);
	TupleDesc	idesc = RelationGetDescr(index);
	int			nkeys = IndexRelationGetNumberOfKeyAttributes(index);
	Datum	   *values;
	bool	   *isnull;
	int			result = 1;

	values = palloc(Max(tdesc->natts, 1) * sizeof(Datum));
	isnull = palloc(Max(tdesc->natts, 1) * sizeof(bool));
	ih_deform_tuple(tdesc, tuple, values, isnull);

	for (int i = 0; i < nkeys && result == 1; i++)
	{
		int			col = index->rd_index->indkey.values[i] - 1;
		CompactAttribute *att = TupleDescCompactAttr(idesc, i);
		bool		inull;
		Datum		ival = index_getattr(dm->itup, i + 1, idesc, &inull);

		if (inull != isnull[col])
			result = 0;
		else if (inull)
			continue;
		else if (!allow_external && att->attlen == -1 &&
				 VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(values[col])))
			result = -1;
		else if (!datum_image_eq(ival, values[col], att->attbyval, att->attlen))
			result = 0;
	}

	pfree(values);
	pfree(isnull);
	return result;
}

/* ------------------------------------------------------------------------
 * For dmbtree (dmbtree_api.h)
 * ------------------------------------------------------------------------
 */

void
iheap_index_fetch_set_entry(IndexFetchTableData *fetch, Relation index,
							IndexTuple itup, bool marked)
{
	IHeapIndexFetchData *f = (IHeapIndexFetchData *) fetch;

	f->xs_dm.index = index;
	f->xs_dm.itup = itup;
	f->xs_dm.marked = marked;
	f->xs_dm.locked = false;
}

/*
 * Index-only scans: does row tid have a version visible to snapshot (an
 * MVCC snapshot) whose key is that of the marked entry itup?  The scan
 * passes over the entry if not, without the executor visiting the row, so
 * a serializable scan records here the rw-conflicts with the writers of
 * versions it does not see, as the fetch of the row would.
 */
bool
iheap_dm_entry_visible(Relation heaprel, ItemPointer tid, Snapshot snapshot,
					   Relation index, IndexTuple itup)
{
	BlockNumber blk = ItemPointerGetBlockNumber(tid);
	OffsetNumber off = ItemPointerGetOffsetNumber(tid);
	IHeapDmEntry dm = {index, itup, true, false};
	bool		ssi = CheckForSerializableConflictOutNeeded(heaprel, snapshot);
	Buffer		buf;
	Page		page;
	ItemId		lp;
	IHeapVersion v;
	int			match = 0;

	if (blk >= RelationGetNumberOfBlocks(heaprel))
		return false;
	buf = ReadBufferExtended(heaprel, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	if (IHeapPageIsData(page) && off >= FirstOffsetNumber &&
		off <= PageGetMaxOffsetNumber(page) &&
		ItemIdIsNormal(lp = PageGetItemId(page, off)) &&
		iheap_resolve_version_ext(heaprel, buf, (char *) page + ItemIdGetOffset(lp),
								  ItemIdGetLength(lp), NULL, snapshot, &v,
								  false, ssi))
	{
		match = iheap_dm_matches(heaprel, &dm, v.tuple, false);
		/* a value stored out of line is fetched with the page unlocked */
		if (match < 0 && v.mem == NULL)
			v.tuple = ih_copy_tuple_aligned(CurrentMemoryContext, v.tuple,
											v.len, &v.mem);
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		if (match < 0)
			match = iheap_dm_matches(heaprel, &dm, v.tuple, true);
		if (v.mem)
			pfree(v.mem);
	}
	else
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buf);
	return match == 1;
}

/*
 * table_index_fetch_tuple_check() for an entry of a dmbtree index, for
 * uniqueness checks (iheap_index_fetch_tuple does the key check).
 */
bool
iheap_dm_fetch_check(Relation heaprel, ItemPointer tid, Snapshot snapshot,
					 Relation index, IndexTuple itup, bool marked,
					 bool *all_dead)
{
	IndexFetchTableData *scan;
	TupleTableSlot *slot;
	bool		call_again = false;
	bool		found;

	slot = table_slot_create(heaprel, NULL);
	scan = table_index_fetch_begin(heaprel, SO_NONE);
	iheap_index_fetch_set_entry(scan, index, itup, marked);
	/* dmbtree's uniqueness check holds the page: the mark is current */
	((IHeapIndexFetchData *) scan)->xs_dm.locked = true;
	found = table_index_fetch_tuple(scan, tid, snapshot, slot, &call_again,
									all_dead);
	table_index_fetch_end(scan);
	ExecDropSingleTupleTableSlot(slot);

	return found;
}

static int
dm_check_cmp(const void *a, const void *b)
{
	const DmbtEntryCheck *ea = (const DmbtEntryCheck *) a;
	const DmbtEntryCheck *eb = (const DmbtEntryCheck *) b;

	return ItemPointerCompare(&ea->tid, &eb->tid);
}

/*
 * The fate of the marked entry (key of dm, row at off of the locked page
 * buf), as described for iheap_dm_check_entries().  Walks the row's
 * versions newest first, over the ones some snapshot may still see: the
 * walk stops below a version whose creator every snapshot sees as
 * committed.  Versions of aborted transactions are seen by nobody.
 *
 * A key value stored out of line is fetched only for the version on the
 * page (if not aborted), whose TOAST data is live; an older version's
 * value may have been replaced by a change that is about to become
 * visible to all, after which VACUUM of the TOAST table may remove it.
 * Such a version makes the answer KEEP, until the walk no longer reaches
 * it.
 */
static DmbtEntryStatus
dm_entry_status(Relation rel, Buffer buf, OffsetNumber off,
				const IHeapDmEntry *dm, GlobalVisState *vistest,
				TransactionId *horizon)
{
	Page		page = BufferGetPage(buf);
	UndoRecPtr	discard = iheap_discard_ptr(rel);
	ItemId		lp;
	IHeapHdr	h;
	char	   *cur;
	uint32		curlen;
	bool		newest = true;	/* no committed or running version above */
	bool		onpage = true;	/* cur is the version on the page */
	bool		fetch_ok = ActiveSnapshotSet();
	int			steps = 0;

	if (!IHeapPageIsData(page) || off < FirstOffsetNumber ||
		off > PageGetMaxOffsetNumber(page))
		return DMBT_ENTRY_DEAD;
	lp = PageGetItemId(page, off);
	if (!ItemIdIsNormal(lp))
		return DMBT_ENTRY_DEAD; /* dead or unused: the row is gone */
	cur = (char *) page + ItemIdGetOffset(lp);
	curlen = ItemIdGetLength(lp);
	ih_hdr_read(cur, &h);

	for (;;)
	{
		bool		deleted = (h.infomask & (IHT_DELETED | IHT_MOVED)) != 0;
		IHeapXidStatus st;
		IHeapUndoRec *rec;
		char	   *mem;
		int			match;

		if (++steps > 100000)
			return DMBT_ENTRY_KEEP;

		if (h.urec < discard)
		{
			/* frozen: visible to everybody, and nothing older is */
			if (deleted)
				return DMBT_ENTRY_DEAD;
			match = iheap_dm_matches(rel, dm, cur, onpage && fetch_ok);
			if (match < 0)
				return DMBT_ENTRY_KEEP;
			if (match == 0)
				return DMBT_ENTRY_DEAD;
			return newest ? DMBT_ENTRY_CURRENT : DMBT_ENTRY_KEEP;
		}

		/* a LOCK record in front of the version is transparent */
		if (h.infomask & IHT_LOCKED)
		{
			rec = iheap_undo_fetch(rel, h.urec, true);
			if (rec == NULL)
			{
				/* discarded under us: committed, visible to all */
				h.urec = InvalidUndoRecPtr;
				continue;
			}
			if (rec->hdr.ur_type == IHU_LOCK)
			{
				IHeapHdr	ph;

				ih_hdr_read(rec->hdr.ur_prev_hdr, &ph);
				h.infomask = ph.infomask | (h.infomask & (IHT_XID_HINTS | IHT_STUB));
				h.urec = ph.urec;
				h.xid = ph.xid;
				pfree(rec);
				continue;
			}
			pfree(rec);
		}

		st = iheap_xid_status(h.xid, InvalidBuffer, NULL);
		if (st != IHX_ABORTED)
		{
			if (!deleted)
			{
				match = iheap_dm_matches(rel, dm, cur, onpage && fetch_ok);
				if (match < 0)
					return DMBT_ENTRY_KEEP;
				if (match > 0)
					return (newest && st == IHX_COMMITTED) ?
						DMBT_ENTRY_CURRENT : DMBT_ENTRY_KEEP;
			}

			/* everybody sees this version or a newer one */
			if (st == IHX_COMMITTED &&
				GlobalVisTestIsRemovableXid(vistest, h.xid, true))
			{
				if (!TransactionIdIsValid(*horizon) ||
					TransactionIdFollows(h.xid, *horizon))
					*horizon = h.xid;
				return DMBT_ENTRY_DEAD;
			}
			newest = false;
		}

		/* step back to the previous version */
		rec = iheap_undo_fetch(rel, h.urec, true);
		if (rec == NULL)
		{
			/*
			 * Discarded under us: its writer is visible to all, so the
			 * version is, as if frozen.  (Discard applies aborted records
			 * before passing them, so an aborted version cannot lose its
			 * record while on the page; be safe if it did.)
			 */
			if (st == IHX_ABORTED)
				return DMBT_ENTRY_KEEP;
			h.urec = InvalidUndoRecPtr;
			continue;
		}
		if (!iheap_rebuild_prior(rel, &cur, &curlen, &h, rec, &mem))
		{
			/* the insert: no version before it ever had the key */
			pfree(rec);
			return DMBT_ENTRY_DEAD;
		}
		pfree(rec);
		onpage = false;
	}
}

TransactionId
iheap_dm_horizon(Relation heaprel)
{
	/* also brings the GlobalVis state the walks test against up to date */
	return GetOldestNonRemovableTransactionId(heaprel);
}

/*
 * dmbtree's cleanup (DESIGN.md §11.5.6): for each delete-marked entry,
 *
 *	DEAD	 no snapshot can see a version of its row with its key: the row
 *			 is gone, or every version some snapshot may see has another key;
 *	CURRENT  the row's newest committed version has its key (and nobody is
 *			 changing the row): the mark can go;
 *	KEEP	 otherwise.
 *
 * The caller holds the index leaf page locked, so the row cannot get a new
 * entry or mark meanwhile: a writer changes the table first (seen here as
 * a running version) and the index after (it waits for the leaf page).
 * entries[] is sorted by TID here.  Returns the conflict horizon for
 * standbys: the newest xid whose version hid a dead entry's key.
 */
TransactionId
iheap_dm_check_entries(Relation rel, Relation index, DmbtEntryCheck *entries,
					   int nentries)
{
	GlobalVisState *vistest = GlobalVisTestFor(rel);
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	TransactionId horizon = InvalidTransactionId;
	MemoryContext cxt;
	MemoryContext oldcxt;
	Buffer		buf = InvalidBuffer;

	qsort(entries, nentries, sizeof(DmbtEntryCheck), dm_check_cmp);

	cxt = AllocSetContextCreate(CurrentMemoryContext, "iheap dmbtree check",
								ALLOCSET_DEFAULT_SIZES);
	for (int i = 0; i < nentries; i++)
	{
		DmbtEntryCheck *e = &entries[i];
		BlockNumber blk = ItemPointerGetBlockNumber(&e->tid);
		IHeapDmEntry dm = {index, e->itup, true, true};

		if (blk >= nblocks)
		{
			e->status = DMBT_ENTRY_DEAD;
			continue;
		}
		if (!BufferIsValid(buf) || BufferGetBlockNumber(buf) != blk)
		{
			if (BufferIsValid(buf))
				UnlockReleaseBuffer(buf);
			buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
		}

		oldcxt = MemoryContextSwitchTo(cxt);
		e->status = dm_entry_status(rel, buf, ItemPointerGetOffsetNumber(&e->tid),
									&dm, vistest, &horizon);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(cxt);
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
	MemoryContextDelete(cxt);

	return horizon;
}

/* ------------------------------------------------------------------------
 * Checking entries against their rows (dmbtree_check)
 * ------------------------------------------------------------------------
 */

static int
dm_verify_cmp(const void *a, const void *b)
{
	const DmbtEntryVerify *ea = (const DmbtEntryVerify *) a;
	const DmbtEntryVerify *eb = (const DmbtEntryVerify *) b;

	return ItemPointerCompare(&ea->tid, &eb->tid);
}

/*
 * Does the newest committed version of the row at "off" of the locked page
 * buf have the key of the unmarked entry dm?  Returns 1 or 0; -1 when there
 * is nothing to compare: no committed version (an insert that is still
 * running or was rolled back), or a deletion or move (the row's entries
 * wait for VACUUM), or a walk that cannot tell.  Returns -2 when the
 * version's key is stored out of line: *copy (and *copymem) are then set to
 * a copy of the version, to compare with the page unlocked.
 *
 * Versions of running transactions, ours included, and of aborted ones are
 * passed over, as in dm_entry_status(), which this follows.
 */
static int
dm_verify_unmarked(Relation rel, Buffer buf, OffsetNumber off,
				   const IHeapDmEntry *dm, char **copy, char **copymem)
{
	Page		page = BufferGetPage(buf);
	UndoRecPtr	discard = iheap_discard_ptr(rel);
	ItemId		lp = PageGetItemId(page, off);
	char	   *cur = (char *) page + ItemIdGetOffset(lp);
	uint32		curlen = ItemIdGetLength(lp);
	char	   *mem = NULL;
	bool		onpage = true;
	int			steps = 0;
	int			match;
	IHeapHdr	h;

	ih_hdr_read(cur, &h);
	for (;;)
	{
		IHeapUndoRec *rec;
		IHeapXidStatus st;
		char	   *newmem;

		if (++steps > 100000)
			return -1;
		if (h.urec < discard)
			break;				/* frozen: committed */

		/* a LOCK record in front of the version is transparent */
		if (h.infomask & IHT_LOCKED)
		{
			rec = iheap_undo_fetch(rel, h.urec, true);
			if (rec == NULL)
				break;			/* discarded under us: committed */
			if (rec->hdr.ur_type == IHU_LOCK)
			{
				IHeapHdr	ph;

				ih_hdr_read(rec->hdr.ur_prev_hdr, &ph);
				h.infomask = ph.infomask | (h.infomask & (IHT_XID_HINTS | IHT_STUB));
				h.urec = ph.urec;
				h.xid = ph.xid;
				pfree(rec);
				continue;
			}
			pfree(rec);
		}

		st = iheap_xid_status(h.xid, InvalidBuffer, NULL);
		if (st == IHX_COMMITTED)
			break;

		/* running, ours or aborted: the version before it */
		rec = iheap_undo_fetch(rel, h.urec, true);
		if (rec == NULL)
		{
			/* discarded under us: committed, unless it is aborted */
			if (st == IHX_ABORTED)
				return -1;
			break;
		}
		if (!iheap_rebuild_prior(rel, &cur, &curlen, &h, rec, &newmem))
		{
			pfree(rec);
			return -1;			/* the insert: nothing committed before it */
		}
		pfree(rec);
		if (mem)
			pfree(mem);
		mem = newmem;
		onpage = false;
	}

	if (h.infomask & (IHT_DELETED | IHT_MOVED))
		return -1;

	match = iheap_dm_matches(rel, dm, cur, false);
	if (match >= 0)
		return match;

	if (onpage)
		*copy = ih_copy_tuple_aligned(CurrentMemoryContext, cur, curlen, copymem);
	else
	{
		*copy = cur;
		*copymem = mem;
	}
	return -2;
}

/*
 * dmbtree_check(checkmarks): check entries of a leaf page against their
 * rows.  Every entry must point to a used line pointer of a data page:
 * line pointers become unused only after index vacuum removed the entries
 * pointing at them (§10.5), and index vacuum needs the leaf page, which the
 * caller holds share-locked.  An unmarked entry must match the newest
 * committed version of its row (§11.5.4).  With the leaf page locked, no
 * entry of the rows can be added or marked meanwhile: a writer changes a row
 * first, a version not committed yet that this check passes over, and the
 * index after.
 *
 * A key stored out of line is compared after the table page is unlocked
 * (TOAST access takes locks and may read), from a copy of the version: the
 * newest committed version's TOAST data stays while the leaf page is locked.
 */
void
iheap_dm_verify_entries(Relation rel, Relation index, DmbtEntryVerify *entries,
						int nentries)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	Buffer		buf = InvalidBuffer;
	MemoryContext cxt;
	MemoryContext oldcxt;
	DmbtEntryVerify **later;
	char	  **latercopy;
	int			nlater = 0;

	qsort(entries, nentries, sizeof(DmbtEntryVerify), dm_verify_cmp);

	cxt = AllocSetContextCreate(CurrentMemoryContext, "iheap dmbtree verify",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);
	later = palloc(Max(nentries, 1) * sizeof(DmbtEntryVerify *));
	latercopy = palloc(Max(nentries, 1) * sizeof(char *));

	for (int i = 0; i < nentries; i++)
	{
		DmbtEntryVerify *e = &entries[i];
		BlockNumber blk = ItemPointerGetBlockNumber(&e->tid);
		OffsetNumber off = ItemPointerGetOffsetNumber(&e->tid);
		IHeapDmEntry dm = {index, e->itup, false, true};
		Page		page;
		ItemId		lp;
		char	   *copy;
		char	   *copymem = NULL;
		int			match;

		e->result = DMBT_VERIFY_OK;
		if (blk >= nblocks)
		{
			e->result = DMBT_VERIFY_NO_ROWS;
			continue;
		}
		if (!BufferIsValid(buf) || BufferGetBlockNumber(buf) != blk)
		{
			if (BufferIsValid(buf))
				UnlockReleaseBuffer(buf);
			buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
		}
		page = BufferGetPage(buf);
		if (!IHeapPageIsData(page))
		{
			e->result = DMBT_VERIFY_NO_ROWS;
			continue;
		}
		if (off < FirstOffsetNumber || off > PageGetMaxOffsetNumber(page) ||
			!ItemIdIsUsed(lp = PageGetItemId(page, off)))
		{
			e->result = DMBT_VERIFY_UNUSED;
			continue;
		}
		if (e->marked || !ItemIdIsNormal(lp))
			continue;			/* LP_DEAD: waiting for index vacuum */

		match = dm_verify_unmarked(rel, buf, off, &dm, &copy, &copymem);
		if (match == 0)
			e->result = DMBT_VERIFY_KEY;
		else if (match == -2)
		{
			later[nlater] = e;
			latercopy[nlater++] = copy;
		}
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);

	for (int i = 0; i < nlater; i++)
	{
		IHeapDmEntry dm = {index, later[i]->itup, false, true};

		if (iheap_dm_matches(rel, &dm, latercopy[i], true) == 0)
			later[i]->result = DMBT_VERIFY_KEY;
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}
