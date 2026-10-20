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
#include "utils/relcache.h"
#include "utils/snapshot.h"

/* ------------------------------------------------------------------------
 * dmbtree, for iheap
 * ------------------------------------------------------------------------
 */

/* is "index" a dmbtree index? */
extern bool dmbtree_is_dmbtree(Relation index);

/*
 * Can "index" take in-place updates of its key columns?  A dmbtree index
 * whose key columns are plain table columns, without a predicate, and
 * whose opclasses all have "equality implies image equality" (the
 * metapage's allequalimage, as for deduplication).
 */
extern bool dmbtree_dm_capable(Relation index);

/*
 * After an in-place update of row "tid" changed the key of "index": add the
 * entry for the new key, delete-marked (with a uniqueness check if
 * "unique"), and delete-mark the entry for the old key.
 */
extern void dmbtree_inplace_insert(Relation index, Relation heaprel,
								   Datum *values, bool *isnull,
								   ItemPointer tid, bool unique);
extern void dmbtree_inplace_mark(Relation index, Relation heaprel,
								 Datum *values, bool *isnull,
								 ItemPointer tid);

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
 * values without visiting the table when the page is all-visible.
 */
extern bool iheap_dm_entry_visible(Relation heaprel, ItemPointer tid,
								   Snapshot snapshot, Relation index,
								   IndexTuple itup);

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
} DmbtEntryStatus;

typedef struct DmbtEntryCheck
{
	ItemPointerData tid;		/* row (unmarked TID) */
	IndexTuple	itup;			/* the entry, for its key */
	int			id;				/* caller's */
	DmbtEntryStatus status;		/* result */
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

#endif							/* DMBTREE_API_H */
