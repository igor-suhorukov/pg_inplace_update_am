/*-------------------------------------------------------------------------
 *
 * dmbtapi.c
 *	  dmbtree's side of dmbtree_api.h: what iheap calls when it updates a
 *	  key column in place (DESIGN.md §11.6), the index property it decides
 *	  that by (§7.4), and the reading of leaf pages iheap_check() does.
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "catalog/pg_index.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"

#include "dmbtree.h"
#include "dmbtree_api.h"

bool
dmbtree_is_dmbtree(Relation index)
{
	return index->rd_rel->relkind == RELKIND_INDEX &&
		index->rd_indam != NULL && index->rd_indam->ambuild == dmbtbuild;
}

/*
 * An index can take in-place updates of the columns it reads when a row's
 * entries can be told apart by key alone: all its key columns' opclasses
 * have "equality implies image equality", as deduplication requires
 * (otherwise two versions could have keys that compare equal but differ in
 * what an index-only scan returns).  Unlike deduplication, INCLUDE columns
 * do not prevent it.  Expressions and a predicate are evaluated by iheap
 * wherever a row version's entry is computed (§21 D24, D39); an INCLUDE
 * column that changes in place leaves the entry marked, with its values.
 */
bool
dmbtree_dm_capable(Relation index)
{
	if (!dmbtree_is_dmbtree(index))
		return false;
	return _dmbt_meta_keys_equalimage(index);
}

void
dmbtree_inplace_insert(Relation index, Relation heaprel, Datum *values,
					   bool *isnull, ItemPointer tid, bool unique)
{
	IndexTuple	itup;

	itup = index_form_tuple(RelationGetDescr(index), values, isnull);
	itup->t_tid = *tid;
	DmbtTidSetMarked(&itup->t_tid, true);
	_dmbt_doinsert_inplace(index, itup, unique, heaprel);
	pfree(itup);
}

void
dmbtree_inplace_mark(Relation index, Relation heaprel, Datum *values,
					 bool *isnull, ItemPointer tid)
{
	IndexTuple	itup;

	itup = index_form_tuple(RelationGetDescr(index), values, isnull);
	itup->t_tid = *tid;
	_dmbt_mark_entry(index, itup, heaprel);
	pfree(itup);
}

StaticAssertDecl(DMBT_DM_FLAG == DMBTREE_TID_MARK,
				 "the build's delete-mark is the delete-mark");

bool
dmbtree_build_older_keys(Relation index, Relation heaprel)
{
	/* the metapage is not there yet: from the operator classes */
	return dmbtree_is_dmbtree(index) && iheap_relation_is_iheap(heaprel) &&
		_dmbt_keys_equalimage(index);
}

/*
 * An in-place update changed only INCLUDE values of the entry of row "tid",
 * which keeps its values (marked): raise the error an insertion of the new
 * entry would raise if it were too large (§21 D39), so that a row whose
 * entry cannot exist is refused as by an update that moves it.
 */
void
dmbtree_check_entry_size(Relation index, Relation heaprel, Datum *values,
						 bool *isnull, ItemPointer tid)
{
	IndexTuple	itup = index_form_tuple(RelationGetDescr(index), values, isnull);
	PGAlignedBlock buf;

	itup->t_tid = *tid;
	/* the check of an insertion on a leaf page */
	_dmbt_pageinit(buf.data, BLCKSZ);
	BTPageGetOpaque(buf.data)->btpo_flags = BTP_LEAF;
	_dmbt_check_third_page(index, heaprel, true, buf.data, itup);
	pfree(itup);
}

/*
 * dmbtree_index_stats(regclass): leaf pages, index tuples, heap TIDs and
 * delete-marked TIDs of a dmbtree index (tests and diagnostics).
 */
PG_FUNCTION_INFO_V1(dmbtree_index_stats);

Datum
dmbtree_index_stats(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Relation	rel;
	BlockNumber nblocks;
	int64		leaf_pages = 0;
	int64		items = 0;
	int64		tids = 0;
	int64		marked = 0;
	int64		dead_items = 0;
	TupleDesc	tupdesc;
	Datum		values[5];
	bool		nulls[5] = {false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	rel = index_open(indexoid, AccessShareLock);
	if (!dmbtree_is_dmbtree(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a dmbtree index",
						RelationGetRelationName(rel))));

	nblocks = RelationGetNumberOfBlocks(rel);
	for (BlockNumber blk = BTREE_METAPAGE + 1; blk < nblocks; blk++)
	{
		Buffer		buf;
		Page		page;
		BTPageOpaque opaque;

		CHECK_FOR_INTERRUPTS();
		buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}
		opaque = BTPageGetOpaque(page);
		if (P_ISLEAF(opaque) && !P_IGNORE(opaque))
		{
			OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

			leaf_pages++;
			for (OffsetNumber off = P_FIRSTDATAKEY(opaque); off <= maxoff;
				 off = OffsetNumberNext(off))
			{
				ItemId		iid = PageGetItemId(page, off);
				IndexTuple	itup = (IndexTuple) PageGetItem(page, iid);

				items++;
				if (ItemIdIsDead(iid))
					dead_items++;
				if (BTreeTupleIsPosting(itup))
				{
					int			n = BTreeTupleGetNPosting(itup);

					tids += n;
					for (int p = 0; p < n; p++)
						if (DmbtTidIsMarked(BTreeTupleGetPostingN(itup, p)))
							marked++;
				}
				else
				{
					tids++;
					if (DmbtTidIsMarked(&itup->t_tid))
						marked++;
				}
			}
		}
		UnlockReleaseBuffer(buf);
	}
	index_close(rel, AccessShareLock);

	values[0] = Int64GetDatum(leaf_pages);
	values[1] = Int64GetDatum(items);
	values[2] = Int64GetDatum(tids);
	values[3] = Int64GetDatum(marked);
	values[4] = Int64GetDatum(dead_items);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* ------------------------------------------------------------------------
 * For iheap_check(): the heap TIDs of leaf pages
 * ------------------------------------------------------------------------
 */

/*
 * The heap TIDs (unmarked) of a leaf page of a btree or dmbtree index,
 * which have nbtree's page layout; -1 if the page is no leaf page in use.
 * The page may be corrupt: nothing is read beyond what its own bounds allow,
 * and malformed items are passed over (verifying the index is the index
 * checkers' job).
 */
static int
leaf_page_heap_tids(Page page, ItemPointerData *tids)
{
	BTPageOpaque opaque;
	OffsetNumber maxoff;
	Size		limit = BLCKSZ - MAXALIGN(sizeof(BTPageOpaqueData));
	int			n = 0;

	if (PageIsNew(page) ||
		PageGetSpecialSize(page) != MAXALIGN(sizeof(BTPageOpaqueData)))
		return -1;
	opaque = BTPageGetOpaque(page);
	if (!P_ISLEAF(opaque) || P_IGNORE(opaque) || P_ISMETA(opaque))
		return -1;

	maxoff = Min(PageGetMaxOffsetNumber(page), MaxIndexTuplesPerPage);
	for (OffsetNumber off = P_FIRSTDATAKEY(opaque); off <= maxoff;
		 off = OffsetNumberNext(off))
	{
		ItemId		iid = PageGetItemId(page, off);
		IndexTuple	itup;

		if (!ItemIdHasStorage(iid) ||
			ItemIdGetLength(iid) < sizeof(IndexTupleData) ||
			ItemIdGetOffset(iid) + ItemIdGetLength(iid) > limit)
			continue;
		itup = (IndexTuple) PageGetItem(page, iid);
		if (IndexTupleSize(itup) > ItemIdGetLength(iid) ||
			BTreeTupleIsPivot(itup))
			continue;
		if (BTreeTupleIsPosting(itup))
		{
			int			np = BTreeTupleGetNPosting(itup);

			if (BTreeTupleGetPostingOffset(itup) + np * sizeof(ItemPointerData) >
				IndexTupleSize(itup))
				continue;
			for (int i = 0; i < np && n < MaxTIDsPerBTreePage; i++)
				tids[n++] = DmbtTidUnmarked(BTreeTupleGetPostingN(itup, i));
		}
		else if (n < MaxTIDsPerBTreePage)
			tids[n++] = DmbtTidUnmarked(&itup->t_tid);
	}
	return n;
}

int
dmbtree_max_leaf_tids(void)
{
	return MaxTIDsPerBTreePage;
}

int
dmbtree_leaf_heap_tids(Relation index, BlockNumber blkno,
					   BufferAccessStrategy strategy, ItemPointerData *tids)
{
	Buffer		buf;
	int			n;

	buf = ReadBufferExtended(index, MAIN_FORKNUM, blkno, RBM_NORMAL, strategy);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	n = leaf_page_heap_tids(BufferGetPage(buf), tids);
	UnlockReleaseBuffer(buf);
	return n;
}

bool
dmbtree_leaf_recheck(Relation index, BlockNumber blkno, ItemPointer tid,
					 bool (*check) (void *arg), void *arg)
{
	ItemPointerData *tids = palloc(MaxTIDsPerBTreePage * sizeof(ItemPointerData));
	Buffer		buf;
	bool		result = true;
	int			n;

	buf = ReadBufferExtended(index, MAIN_FORKNUM, blkno, RBM_NORMAL, NULL);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	n = leaf_page_heap_tids(BufferGetPage(buf), tids);
	for (int i = 0; i < n; i++)
	{
		if (ItemPointerEquals(&tids[i], tid))
		{
			result = check(arg);
			break;
		}
	}
	UnlockReleaseBuffer(buf);
	pfree(tids);
	return result;
}
