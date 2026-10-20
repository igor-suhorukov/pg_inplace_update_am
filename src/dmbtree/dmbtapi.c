/*-------------------------------------------------------------------------
 *
 * dmbtapi.c
 *	  dmbtree's side of dmbtree_api.h: what iheap calls when it updates a
 *	  key column in place (DESIGN.md §11.6), and the index property it
 *	  decides that by (§7.4).
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
 * An index can take in-place updates of its key columns when a row's
 * entries can be told apart by key alone:
 *
 *	- its key columns are table columns, not expressions, and it has no
 *	  predicate (a row's key is then just some of its column values, and
 *	  every version of the row has an entry; §21 D24);
 *	- all its key columns' opclasses have "equality implies image
 *	  equality", as deduplication requires (otherwise two versions could
 *	  have keys that compare equal but differ in what an index-only scan
 *	  returns).  Unlike deduplication, INCLUDE columns do not prevent it.
 *
 * INCLUDE columns are fine: iheap moves the row when one of them changes.
 */
bool
dmbtree_dm_capable(Relation index)
{
	if (!dmbtree_is_dmbtree(index))
		return false;
	if (!heap_attisnull(index->rd_indextuple, Anum_pg_index_indexprs, NULL) ||
		!heap_attisnull(index->rd_indextuple, Anum_pg_index_indpred, NULL))
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
