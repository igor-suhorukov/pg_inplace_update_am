/*-------------------------------------------------------------------------
 *
 * dmbtree_api.h
 *	  What iheap and dmbtree call of each other (DESIGN.md §11.5, §11.6).
 *
 * Both access methods live in one library.  iheap keeps dmbtree indexes
 * up to date when it updates a key column in place, and dmbtree hands the
 * entries its scans return to iheap, which checks a row's key against the
 * entry that led to it.  Nothing here exposes either AM's internals.
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#ifndef DMBTREE_API_H
#define DMBTREE_API_H

#include "access/itup.h"
#include "access/relscan.h"
#include "storage/buf.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

/* ------------------------------------------------------------------------
 * dmbtree, for iheap
 * ------------------------------------------------------------------------
 */

/* is "index" a dmbtree index? */
extern bool dmbtree_is_dmbtree(Relation index);

/*
 * Can "index" take in-place updates of the columns it reads?  A dmbtree
 * index whose key columns' opclasses all have "equality implies image
 * equality" (computed at build time for the key columns, as for
 * deduplication); expressions, a predicate and INCLUDE columns are fine
 * (§21 D24, D39).
 */
extern bool dmbtree_dm_capable(Relation index);

/*
 * After an in-place update of row "tid" changed the entry of "index": add the
 * entry for the new key, delete-marked (with a uniqueness check if
 * "unique"), and delete-mark the entry for the old key.
 */
extern void dmbtree_inplace_insert(Relation index, Relation heaprel,
								   Datum *values, bool *isnull,
								   ItemPointer tid, bool unique);
extern void dmbtree_inplace_mark(Relation index, Relation heaprel,
								 Datum *values, bool *isnull,
								 ItemPointer tid);

/*
 * An in-place update changed only the INCLUDE values of row "tid"'s entry,
 * which is marked and keeps its values (§21 D39): raise the error an
 * insertion of the new entry would raise if it is too large.
 */
extern void dmbtree_check_entry_size(Relation index, Relation heaprel,
									 Datum *values, bool *isnull,
									 ItemPointer tid);

/*
 * Does a build of "index" (not a concurrent one) on "heaprel" take the keys
 * of older row versions that some snapshot may still see, as delete-marked
 * entries (§21 D32 B)?  A dmbtree index that can take in-place updates of
 * the columns it reads, on an iheap table.  The table AM's build scan hands
 * such entries to the build callback with DMBTREE_TID_MARK set in the TID's
 * offset number; dmbtree inserts them after the sorted load (an entry the
 * load put there already is marked instead).
 */
extern bool dmbtree_build_older_keys(Relation index, Relation heaprel);

#define DMBTREE_TID_MARK	((OffsetNumber) 0x8000)

extern void dmbtree_rmgr_register(void);

/* ------------------------------------------------------------------------
 * iheap, for dmbtree
 * ------------------------------------------------------------------------
 */

extern bool iheap_relation_is_iheap(Relation rel);

/*
 * The entry an index scan is about to hand to the executor: the next
 * iheap_index_fetch_tuple() through "fetch" checks the version it finds
 * against it (when it is marked, or the version came from undo).
 */
extern void iheap_index_fetch_set_entry(IndexFetchTableData *fetch,
										Relation index, IndexTuple itup,
										bool marked);

/*
 * Does row "tid" have a version visible to "snapshot" whose key is the one
 * of entry "itup"?  For index-only scans, which take the index tuple's
 * values without visiting the table when the page is all-visible.  If
 * "subst" is not NULL and that version's INCLUDE values differ from the
 * entry's, *subst is set to an index tuple with the version's values, for
 * the scan to return instead (palloc'd).
 */
extern bool iheap_dm_entry_visible(Relation heaprel, ItemPointer tid,
								   Snapshot snapshot, Relation index,
								   IndexTuple itup, IndexTuple *subst);

/*
 * table_index_fetch_tuple_check() for a dmbtree entry: uniqueness checks
 * with SnapshotDirty, where a row whose key is being changed away from the
 * entry's key by a running transaction still conflicts (until it commits).
 */
extern bool iheap_dm_fetch_check(Relation heaprel, ItemPointer tid,
								 Snapshot snapshot, Relation index,
								 IndexTuple itup, bool marked,
								 bool *all_dead);

/* the fate of a delete-marked entry (cleanup) */
typedef enum DmbtEntryStatus
{
	DMBT_ENTRY_KEEP = 0,		/* some snapshot may need it, or undecided */
	DMBT_ENTRY_DEAD,			/* no snapshot can need it: remove it */
	DMBT_ENTRY_CURRENT,			/* the newest committed version has its
								 * key: the mark can be cleared */
	DMBT_ENTRY_REWRITE,			/* the same, but other INCLUDE values,
								 * and every snapshot sees that version:
								 * the entry can take them (newtup) and
								 * lose its mark */
} DmbtEntryStatus;

typedef struct DmbtEntryCheck
{
	ItemPointerData tid;		/* row (unmarked TID) */
	IndexTuple	itup;			/* the entry, for its key */
	int			id;				/* caller's */
	DmbtEntryStatus status;		/* result */
	TransactionId wait;			/* KEEP: the writer of the row's newest
								 * version; until every snapshot sees it,
								 * the entry stays undecided (or invalid:
								 * unknown) */
	IndexTuple	newtup;			/* REWRITE: the entry to put in its place
								 * (unmarked TID; palloc'd) */
} DmbtEntryCheck;

/*
 * Decide the fate of delete-marked entries of "index" (DESIGN.md §11.5.6).
 * Returns the newest xid whose changes made a dead entry unneeded, the
 * conflict horizon for standbys (InvalidTransactionId if none).
 */
extern TransactionId iheap_dm_check_entries(Relation heaprel, Relation index,
											DmbtEntryCheck *entries,
											int nentries);

/*
 * The horizon iheap_dm_check_entries() decides by: changes of transactions
 * before it are visible to every snapshot.  Entries it keeps stay kept
 * until it moves past them.
 */
extern TransactionId iheap_dm_horizon(Relation heaprel);

/* what dmbtree_check(checkmarks) finds out about an entry */
typedef enum DmbtVerifyResult
{
	DMBT_VERIFY_OK = 0,
	DMBT_VERIFY_NO_ROWS,		/* its block holds no rows */
	DMBT_VERIFY_UNUSED,			/* its line pointer is not in use */
	DMBT_VERIFY_KEY,			/* unmarked, and the newest committed version
								 * of its row has another key */
} DmbtVerifyResult;

typedef struct DmbtEntryVerify
{
	ItemPointerData tid;		/* row (unmarked TID) */
	IndexTuple	itup;			/* the entry, for its key */
	bool		marked;
	OffsetNumber offnum;		/* caller's: where the entry is */
	DmbtVerifyResult result;	/* result */
} DmbtEntryVerify;

/*
 * dmbtree_check(checkmarks): check the entries of a leaf page, which the
 * caller holds share-locked, against their rows (DESIGN.md §11.5.4, §21
 * D34).  entries[] is sorted by TID here.
 */
extern void iheap_dm_verify_entries(Relation heaprel, Relation index,
									DmbtEntryVerify *entries, int nentries);

/* ------------------------------------------------------------------------
 * dmbtree, for iheap_check()
 * ------------------------------------------------------------------------
 */

/*
 * The heap TIDs (unmarked) of the entries of leaf page blkno of a btree or
 * dmbtree index, which have nbtree's page layout.  Returns how many were
 * written to tids[] (room for MaxTIDsPerBTreePage), or -1 when the block is
 * not a leaf page in use.  Only what the page's own bounds allow is read.
 * dmbtree_max_leaf_tids() is the room a leaf page can need.
 */
extern int	dmbtree_max_leaf_tids(void);
extern int	dmbtree_leaf_heap_tids(Relation index, BlockNumber blkno,
								   BufferAccessStrategy strategy,
								   ItemPointerData *tids);

/*
 * Is tid still the heap TID of an entry of leaf page blkno?  If so, calls
 * check(arg) with the page share-locked, so that index vacuum cannot remove
 * the entry meanwhile, and returns its result; returns true otherwise.
 */
extern bool dmbtree_leaf_recheck(Relation index, BlockNumber blkno,
								 ItemPointer tid,
								 bool (*check) (void *arg), void *arg);

#endif							/* DMBTREE_API_H */
