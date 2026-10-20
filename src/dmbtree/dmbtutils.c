/*-------------------------------------------------------------------------
 *
 * nbtutils.c
 *	  Utility code for Postgres btree implementation.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtutils.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <time.h>

#include "dmbtree.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/relscan.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/pg_index.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "commands/progress.h"
#include "common/int.h"
#include "lib/qunique.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "storage/subsystems.h"
#include "utils/datum.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


static int	_dmbt_compare_int(const void *va, const void *vb);
static int	_dmbt_keep_natts(Relation rel, IndexTuple lastleft,
						   IndexTuple firstright, BTScanInsert itup_key);


/*
 * _dmbt_mkscankey
 *		Build an insertion scan key that contains comparison data from itup
 *		as well as comparator routines appropriate to the key datatypes.
 *
 *		The result is intended for use with _dmbt_compare() and _dmbt_truncate().
 *		Callers that don't need to fill out the insertion scankey arguments
 *		(e.g. they use an ad-hoc comparison routine, or only need a scankey
 *		for _dmbt_truncate()) can pass a NULL index tuple.  The scankey will
 *		be initialized as if an "all truncated" pivot tuple was passed
 *		instead.
 *
 *		Note that we may occasionally have to share lock the metapage to
 *		determine whether or not the keys in the index are expected to be
 *		unique (i.e. if this is a "heapkeyspace" index).  We assume a
 *		heapkeyspace index when caller passes a NULL tuple, allowing index
 *		build callers to avoid accessing the non-existent metapage.  We
 *		also assume that the index is _not_ allequalimage when a NULL tuple
 *		is passed; CREATE INDEX callers call _dmbt_allequalimage() to set the
 *		field themselves.
 */
BTScanInsert
_dmbt_mkscankey(Relation rel, IndexTuple itup)
{
	BTScanInsert key;
	ScanKey		skey;
	TupleDesc	itupdesc;
	int			indnkeyatts;
	int16	   *indoption;
	int			tupnatts;
	int			i;

	itupdesc = RelationGetDescr(rel);
	indnkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
	indoption = rel->rd_indoption;
	tupnatts = itup ? BTreeTupleGetNAtts(itup, rel) : 0;

	Assert(tupnatts <= IndexRelationGetNumberOfAttributes(rel));

	/*
	 * We'll execute search using scan key constructed on key columns.
	 * Truncated attributes and non-key attributes are omitted from the final
	 * scan key.
	 */
	key = palloc(offsetof(BTScanInsertData, scankeys) +
				 sizeof(ScanKeyData) * indnkeyatts);
	if (itup)
		_dmbt_metaversion(rel, &key->heapkeyspace, &key->allequalimage);
	else
	{
		/* Utility statement callers can set these fields themselves */
		key->heapkeyspace = true;
		key->allequalimage = false;
	}
	key->anynullkeys = false;	/* initial assumption */
	key->nextkey = false;		/* usual case, required by dmbtinsert */
	key->backward = false;		/* usual case, required by dmbtinsert */
	key->keysz = Min(indnkeyatts, tupnatts);
	key->scantid = key->heapkeyspace && itup ?
		BTreeTupleGetHeapTID(itup) : NULL;
	skey = key->scankeys;
	for (i = 0; i < indnkeyatts; i++)
	{
		FmgrInfo   *procinfo;
		Datum		arg;
		bool		null;
		int			flags;

		/*
		 * We can use the cached (default) support procs since no cross-type
		 * comparison can be needed.
		 */
		procinfo = index_getprocinfo(rel, i + 1, BTORDER_PROC);

		/*
		 * Key arguments built from truncated attributes (or when caller
		 * provides no tuple) are defensively represented as NULL values. They
		 * should never be used.
		 */
		if (i < tupnatts)
			arg = index_getattr(itup, i + 1, itupdesc, &null);
		else
		{
			arg = (Datum) 0;
			null = true;
		}
		flags = (null ? SK_ISNULL : 0) | (indoption[i] << SK_BT_INDOPTION_SHIFT);
		ScanKeyEntryInitializeWithInfo(&skey[i],
									   flags,
									   (AttrNumber) (i + 1),
									   InvalidStrategy,
									   InvalidOid,
									   rel->rd_indcollation[i],
									   procinfo,
									   arg);
		/* Record if any key attribute is NULL (or truncated) */
		if (null)
			key->anynullkeys = true;
	}

	/*
	 * In NULLS NOT DISTINCT mode, we pretend that there are no null keys, so
	 * that full uniqueness check is done.
	 */
	if (rel->rd_index->indnullsnotdistinct)
		key->anynullkeys = false;

	return key;
}

/*
 * qsort comparison function for int arrays
 */
static int
_dmbt_compare_int(const void *va, const void *vb)
{
	int			a = *((const int *) va);
	int			b = *((const int *) vb);

	return pg_cmp_s32(a, b);
}

/*
 * _dmbt_killitems - set LP_DEAD state for items an indexscan caller has
 * told us were killed
 *
 * scan->opaque, referenced locally through so, contains information about the
 * current page and killed tuples thereon (generally, this should only be
 * called if so->numKilled > 0).
 *
 * Caller should not have a lock on the so->currPos page, but must hold a
 * buffer pin when !so->dropPin.  When we return, it still won't be locked.
 * It'll continue to hold whatever pins were held before calling here.
 *
 * We match items by heap TID before assuming they are the right ones to set
 * LP_DEAD.  If the scan is one that holds a buffer pin on the target page
 * continuously from initially reading the items until applying this function
 * (if it is a !so->dropPin scan), VACUUM cannot have deleted any items on the
 * page, so the page's TIDs can't have been recycled by now.  There's no risk
 * that we'll confuse a new index tuple that happens to use a recycled TID
 * with a now-removed tuple with the same TID (that used to be on this same
 * page).  We can't rely on that during scans that drop buffer pins eagerly
 * (so->dropPin scans), though, so we must condition setting LP_DEAD bits on
 * the page LSN having not changed since back when _dmbt_readpage saw the page.
 * We totally give up on setting LP_DEAD bits when the page LSN changed.
 *
 * We give up much less often during !so->dropPin scans, but it still happens.
 * We cope with cases where items have moved right due to insertions.  If an
 * item has moved off the current page due to a split, we'll fail to find it
 * and just give up on it.
 */
void
_dmbt_killitems(IndexScanDesc scan)
{
	Relation	rel = scan->indexRelation;
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Page		page;
	BTPageOpaque opaque;
	OffsetNumber minoff;
	OffsetNumber maxoff;
	int			numKilled = so->numKilled;
	bool		killedsomething = false;
	Buffer		buf;

	Assert(numKilled > 0);
	Assert(BTScanPosIsValid(so->currPos));
	Assert(scan->heapRelation != NULL); /* can't be a bitmap index scan */

	/* Always invalidate so->killedItems[] before leaving so->currPos */
	so->numKilled = 0;

	/*
	 * We need to iterate through so->killedItems[] in leaf page order; the
	 * loop below expects this (when marking posting list tuples, at least).
	 * so->killedItems[] is now in whatever order the scan returned items in.
	 * Scrollable cursor scans might have even saved the same item/TID twice.
	 *
	 * Sort and unique-ify so->killedItems[] to deal with all this.
	 */
	if (numKilled > 1)
	{
		qsort(so->killedItems, numKilled, sizeof(int), _dmbt_compare_int);
		numKilled = qunique(so->killedItems, numKilled, sizeof(int),
							_dmbt_compare_int);
	}

	if (!so->dropPin)
	{
		/*
		 * We have held the pin on this page since we read the index tuples,
		 * so all we need to do is lock it.  The pin will have prevented
		 * concurrent VACUUMs from recycling any of the TIDs on the page.
		 */
		Assert(BTScanPosIsPinned(so->currPos));
		buf = so->currPos.buf;
		_dmbt_lockbuf(rel, buf, BT_READ);
	}
	else
	{
		XLogRecPtr	latestlsn;

		Assert(!BTScanPosIsPinned(so->currPos));
		buf = _dmbt_getbuf(rel, so->currPos.currPage, BT_READ);

		latestlsn = BufferGetLSNAtomic(buf);
		Assert(so->currPos.lsn <= latestlsn);
		if (so->currPos.lsn != latestlsn)
		{
			/* Modified, give up on hinting */
			_dmbt_relbuf(rel, buf);
			return;
		}

		/* Unmodified, hinting is safe */
	}

	page = BufferGetPage(buf);
	opaque = BTPageGetOpaque(page);
	minoff = P_FIRSTDATAKEY(opaque);
	maxoff = PageGetMaxOffsetNumber(page);

	/* Iterate through so->killedItems[] in leaf page order */
	for (int i = 0; i < numKilled; i++)
	{
		int			itemIndex = so->killedItems[i];
		BTScanPosItem *kitem = &so->currPos.items[itemIndex];
		OffsetNumber offnum = kitem->indexOffset;

		Assert(itemIndex >= so->currPos.firstItem &&
			   itemIndex <= so->currPos.lastItem);
		Assert(i == 0 ||
			   offnum >= so->currPos.items[so->killedItems[i - 1]].indexOffset);

		if (offnum < minoff)
			continue;			/* pure paranoia */
		while (offnum <= maxoff)
		{
			ItemId		iid = PageGetItemId(page, offnum);
			IndexTuple	ituple = (IndexTuple) PageGetItem(page, iid);
			bool		killtuple = false;

			if (BTreeTupleIsPosting(ituple))
			{
				int			pi = i + 1;
				int			nposting = BTreeTupleGetNPosting(ituple);
				int			j;

				/*
				 * Note that the page may have been modified in almost any way
				 * since we first read it (in the !so->dropPin case), so it's
				 * possible that this posting list tuple wasn't a posting list
				 * tuple when we first encountered its heap TIDs.
				 */
				for (j = 0; j < nposting; j++)
				{
					ItemPointer item = BTreeTupleGetPostingN(ituple, j);

					/* dmbtree: the delete-mark may have changed meanwhile */
					if (!DmbtTidEquals(item, &kitem->heapTid))
						break;	/* out of posting list loop */

					/*
					 * kitem must have matching offnum when heap TIDs match,
					 * though only in the common case where the page can't
					 * have been concurrently modified
					 */
					Assert(kitem->indexOffset == offnum || !so->dropPin);

					/*
					 * Read-ahead to later kitems here.
					 *
					 * We rely on the assumption that not advancing kitem here
					 * will prevent us from considering the posting list tuple
					 * fully dead by not matching its next heap TID in next
					 * loop iteration.
					 *
					 * If, on the other hand, this is the final heap TID in
					 * the posting list tuple, then tuple gets killed
					 * regardless (i.e. we handle the case where the last
					 * kitem is also the last heap TID in the last index tuple
					 * correctly -- posting tuple still gets killed).
					 */
					if (pi < numKilled)
						kitem = &so->currPos.items[so->killedItems[pi++]];
				}

				/*
				 * Don't bother advancing the outermost loop's int iterator to
				 * avoid processing killed items that relate to the same
				 * offnum/posting list tuple.  This micro-optimization hardly
				 * seems worth it.  (Further iterations of the outermost loop
				 * will fail to match on this same posting list's first heap
				 * TID instead, so we'll advance to the next offnum/index
				 * tuple pretty quickly.)
				 */
				if (j == nposting)
					killtuple = true;
			}
			else if (DmbtTidEquals(&ituple->t_tid, &kitem->heapTid))
				killtuple = true;

			/*
			 * Mark index item as dead, if it isn't already.  Since this
			 * happens while holding a buffer lock possibly in shared mode,
			 * it's possible that multiple processes attempt to do this
			 * simultaneously, leading to multiple full-page images being sent
			 * to WAL (if wal_log_hints or data checksums are enabled), which
			 * is undesirable.
			 */
			if (killtuple && !ItemIdIsDead(iid))
			{
				if (!killedsomething)
				{
					/*
					 * Use the hint bit infrastructure to check if we can
					 * update the page while just holding a share lock. If we
					 * are not allowed, there's no point continuing.
					 */
					if (!BufferBeginSetHintBits(buf))
						goto unlock_page;
				}

				/* found the item/all posting list items */
				ItemIdMarkDead(iid);
				killedsomething = true;
				break;			/* out of inner search loop */
			}
			offnum = OffsetNumberNext(offnum);
		}
	}

	/*
	 * Since this can be redone later if needed, mark as dirty hint.
	 *
	 * Whenever we mark anything LP_DEAD, we also set the page's
	 * BTP_HAS_GARBAGE flag, which is likewise just a hint.  (Note that we
	 * only rely on the page-level flag in !heapkeyspace indexes.)
	 */
	if (killedsomething)
	{
		opaque->btpo_flags |= BTP_HAS_GARBAGE;
		BufferFinishSetHintBits(buf, true, true);
	}

unlock_page:
	if (!so->dropPin)
		_dmbt_unlockbuf(rel, buf);
	else
		_dmbt_relbuf(rel, buf);
}


/*
 * dmbtree: the vacuum cycle-id bookkeeping (_bt_vacuum_cycleid,
 * _bt_start_vacuum, _bt_end_vacuum, _bt_end_vacuum_callback and their shared
 * memory) is core's: it is keyed by relation, so dmbtree indexes share
 * nbtree's array.  btoptions, btproperty and btbuildphasename are core's
 * too (the reloptions are btree's).
 */

/*
 *	_dmbt_truncate() -- create tuple without unneeded suffix attributes.
 *
 * Returns truncated pivot index tuple allocated in caller's memory context,
 * with key attributes copied from caller's firstright argument.  If rel is
 * an INCLUDE index, non-key attributes will definitely be truncated away,
 * since they're not part of the key space.  More aggressive suffix
 * truncation can take place when it's clear that the returned tuple does not
 * need one or more suffix key attributes.  We only need to keep firstright
 * attributes up to and including the first non-lastleft-equal attribute.
 * Caller's insertion scankey is used to compare the tuples; the scankey's
 * argument values are not considered here.
 *
 * Note that returned tuple's t_tid offset will hold the number of attributes
 * present, so the original item pointer offset is not represented.  Caller
 * should only change truncated tuple's downlink.  Note also that truncated
 * key attributes are treated as containing "minus infinity" values by
 * _dmbt_compare().
 *
 * In the worst case (when a heap TID must be appended to distinguish lastleft
 * from firstright), the size of the returned tuple is the size of firstright
 * plus the size of an additional MAXALIGN()'d item pointer.  This guarantee
 * is important, since callers need to stay under the 1/3 of a page
 * restriction on tuple size.  If this routine is ever taught to truncate
 * within an attribute/datum, it will need to avoid returning an enlarged
 * tuple to caller when truncation + TOAST compression ends up enlarging the
 * final datum.
 */
IndexTuple
_dmbt_truncate(Relation rel, IndexTuple lastleft, IndexTuple firstright,
			 BTScanInsert itup_key)
{
	TupleDesc	itupdesc = RelationGetDescr(rel);
	int16		nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
	int			keepnatts;
	IndexTuple	pivot;
	IndexTuple	tidpivot;
	ItemPointer pivotheaptid;
	Size		newsize;

	/*
	 * We should only ever truncate non-pivot tuples from leaf pages.  It's
	 * never okay to truncate when splitting an internal page.
	 */
	Assert(!BTreeTupleIsPivot(lastleft) && !BTreeTupleIsPivot(firstright));

	/* Determine how many attributes must be kept in truncated tuple */
	keepnatts = _dmbt_keep_natts(rel, lastleft, firstright, itup_key);

#ifdef DEBUG_NO_TRUNCATE
	/* Force truncation to be ineffective for testing purposes */
	keepnatts = nkeyatts + 1;
#endif

	pivot = index_truncate_tuple(itupdesc, firstright,
								 Min(keepnatts, nkeyatts));

	if (BTreeTupleIsPosting(pivot))
	{
		/*
		 * index_truncate_tuple() just returns a straight copy of firstright
		 * when it has no attributes to truncate.  When that happens, we may
		 * need to truncate away a posting list here instead.
		 */
		Assert(keepnatts == nkeyatts || keepnatts == nkeyatts + 1);
		Assert(IndexRelationGetNumberOfAttributes(rel) == nkeyatts);
		pivot->t_info &= ~INDEX_SIZE_MASK;
		pivot->t_info |= MAXALIGN(BTreeTupleGetPostingOffset(firstright));
	}

	/*
	 * If there is a distinguishing key attribute within pivot tuple, we're
	 * done
	 */
	if (keepnatts <= nkeyatts)
	{
		BTreeTupleSetNAtts(pivot, keepnatts, false);
		return pivot;
	}

	/*
	 * We have to store a heap TID in the new pivot tuple, since no non-TID
	 * key attribute value in firstright distinguishes the right side of the
	 * split from the left side.  nbtree conceptualizes this case as an
	 * inability to truncate away any key attributes, since heap TID is
	 * treated as just another key attribute (despite lacking a pg_attribute
	 * entry).
	 *
	 * Use enlarged space that holds a copy of pivot.  We need the extra space
	 * to store a heap TID at the end (using the special pivot tuple
	 * representation).  Note that the original pivot already has firstright's
	 * possible posting list/non-key attribute values removed at this point.
	 */
	newsize = MAXALIGN(IndexTupleSize(pivot)) + MAXALIGN(sizeof(ItemPointerData));
	tidpivot = palloc0(newsize);
	memcpy(tidpivot, pivot, MAXALIGN(IndexTupleSize(pivot)));
	/* Cannot leak memory here */
	pfree(pivot);

	/*
	 * Store all of firstright's key attribute values plus a tiebreaker heap
	 * TID value in enlarged pivot tuple
	 */
	tidpivot->t_info &= ~INDEX_SIZE_MASK;
	tidpivot->t_info |= newsize;
	BTreeTupleSetNAtts(tidpivot, nkeyatts, true);
	pivotheaptid = BTreeTupleGetHeapTID(tidpivot);

	/*
	 * Lehman & Yao use lastleft as the leaf high key in all cases, but don't
	 * consider suffix truncation.  It seems like a good idea to follow that
	 * example in cases where no truncation takes place -- use lastleft's heap
	 * TID.  (This is also the closest value to negative infinity that's
	 * legally usable.)
	 */
	ItemPointerCopy(BTreeTupleGetMaxHeapTID(lastleft), pivotheaptid);
	DmbtTidSetMarked(pivotheaptid, false);	/* dmbtree: pivots carry none */

	/*
	 * We're done.  Assert() that heap TID invariants hold before returning.
	 *
	 * Lehman and Yao require that the downlink to the right page, which is to
	 * be inserted into the parent page in the second phase of a page split be
	 * a strict lower bound on items on the right page, and a non-strict upper
	 * bound for items on the left page.  Assert that heap TIDs follow these
	 * invariants, since a heap TID value is apparently needed as a
	 * tiebreaker.
	 */
#ifndef DEBUG_NO_TRUNCATE
	Assert(DmbtTidCompare(BTreeTupleGetMaxHeapTID(lastleft),
							  BTreeTupleGetHeapTID(firstright)) < 0);
	Assert(DmbtTidCompare(pivotheaptid,
							  BTreeTupleGetHeapTID(lastleft)) >= 0);
	Assert(DmbtTidCompare(pivotheaptid,
							  BTreeTupleGetHeapTID(firstright)) < 0);
#else

	/*
	 * Those invariants aren't guaranteed to hold for lastleft + firstright
	 * heap TID attribute values when they're considered here only because
	 * DEBUG_NO_TRUNCATE is defined (a heap TID is probably not actually
	 * needed as a tiebreaker).  DEBUG_NO_TRUNCATE must therefore use a heap
	 * TID value that always works as a strict lower bound for items to the
	 * right.  In particular, it must avoid using firstright's leading key
	 * attribute values along with lastleft's heap TID value when lastleft's
	 * TID happens to be greater than firstright's TID.
	 */
	ItemPointerCopy(BTreeTupleGetHeapTID(firstright), pivotheaptid);
	DmbtTidSetMarked(pivotheaptid, false);	/* dmbtree: pivots carry none */

	/*
	 * Pivot heap TID should never be fully equal to firstright.  Note that
	 * the pivot heap TID will still end up equal to lastleft's heap TID when
	 * that's the only usable value.
	 */
	ItemPointerSetOffsetNumber(pivotheaptid,
							   OffsetNumberPrev(ItemPointerGetOffsetNumber(pivotheaptid)));
	Assert(DmbtTidCompare(pivotheaptid,
							  BTreeTupleGetHeapTID(firstright)) < 0);
#endif

	return tidpivot;
}

/*
 * _dmbt_keep_natts - how many key attributes to keep when truncating.
 *
 * Caller provides two tuples that enclose a split point.  Caller's insertion
 * scankey is used to compare the tuples; the scankey's argument values are
 * not considered here.
 *
 * This can return a number of attributes that is one greater than the
 * number of key attributes for the index relation.  This indicates that the
 * caller must use a heap TID as a unique-ifier in new pivot tuple.
 */
static int
_dmbt_keep_natts(Relation rel, IndexTuple lastleft, IndexTuple firstright,
			   BTScanInsert itup_key)
{
	int			nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
	TupleDesc	itupdesc = RelationGetDescr(rel);
	int			keepnatts;
	ScanKey		scankey;

	/*
	 * _dmbt_compare() treats truncated key attributes as having the value minus
	 * infinity, which would break searches within !heapkeyspace indexes.  We
	 * must still truncate away non-key attribute values, though.
	 */
	if (!itup_key->heapkeyspace)
		return nkeyatts;

	scankey = itup_key->scankeys;
	keepnatts = 1;
	for (int attnum = 1; attnum <= nkeyatts; attnum++, scankey++)
	{
		Datum		datum1,
					datum2;
		bool		isNull1,
					isNull2;

		datum1 = index_getattr(lastleft, attnum, itupdesc, &isNull1);
		datum2 = index_getattr(firstright, attnum, itupdesc, &isNull2);

		if (isNull1 != isNull2)
			break;

		if (!isNull1 &&
			DatumGetInt32(FunctionCall2Coll(&scankey->sk_func,
											scankey->sk_collation,
											datum1,
											datum2)) != 0)
			break;

		keepnatts++;
	}

	/*
	 * Assert that _dmbt_keep_natts_fast() agrees with us in passing.  This is
	 * expected in an allequalimage index.
	 */
	Assert(!itup_key->allequalimage ||
		   keepnatts == _dmbt_keep_natts_fast(rel, lastleft, firstright));

	return keepnatts;
}

/*
 * _dmbt_keep_natts_fast - fast bitwise variant of _dmbt_keep_natts.
 *
 * This is exported so that a candidate split point can have its effect on
 * suffix truncation inexpensively evaluated ahead of time when finding a
 * split location.  A naive bitwise approach to datum comparisons is used to
 * save cycles.
 *
 * The approach taken here usually provides the same answer as _dmbt_keep_natts
 * will (for the same pair of tuples from a heapkeyspace index), since the
 * majority of btree opclasses can never indicate that two datums are equal
 * unless they're bitwise equal after detoasting.  When an index only has
 * "equal image" columns, routine is guaranteed to give the same result as
 * _dmbt_keep_natts would.
 *
 * Callers can rely on the fact that attributes considered equal here are
 * definitely also equal according to _dmbt_keep_natts, even when the index uses
 * an opclass or collation that is not "allequalimage"/deduplication-safe.
 * This weaker guarantee is good enough for nbtsplitloc.c caller, since false
 * negatives generally only have the effect of making leaf page splits use a
 * more balanced split point.
 */
int
_dmbt_keep_natts_fast(Relation rel, IndexTuple lastleft, IndexTuple firstright)
{
	TupleDesc	itupdesc = RelationGetDescr(rel);
	int			keysz = IndexRelationGetNumberOfKeyAttributes(rel);
	int			keepnatts;

	keepnatts = 1;
	for (int attnum = 1; attnum <= keysz; attnum++)
	{
		Datum		datum1,
					datum2;
		bool		isNull1,
					isNull2;
		CompactAttribute *att;

		datum1 = index_getattr(lastleft, attnum, itupdesc, &isNull1);
		datum2 = index_getattr(firstright, attnum, itupdesc, &isNull2);
		att = TupleDescCompactAttr(itupdesc, attnum - 1);

		if (isNull1 != isNull2)
			break;

		if (!isNull1 &&
			!datum_image_eq(datum1, datum2, att->attbyval, att->attlen))
			break;

		keepnatts++;
	}

	return keepnatts;
}

/*
 *  _dmbt_check_natts() -- Verify tuple has expected number of attributes.
 *
 * Returns value indicating if the expected number of attributes were found
 * for a particular offset on page.  This can be used as a general purpose
 * sanity check.
 *
 * Testing a tuple directly with BTreeTupleGetNAtts() should generally be
 * preferred to calling here.  That's usually more convenient, and is always
 * more explicit.  Call here instead when offnum's tuple may be a negative
 * infinity tuple that uses the pre-v11 on-disk representation, or when a low
 * context check is appropriate.  This routine is as strict as possible about
 * what is expected on each version of btree.
 */
bool
_dmbt_check_natts(Relation rel, bool heapkeyspace, Page page, OffsetNumber offnum)
{
	int16		natts = IndexRelationGetNumberOfAttributes(rel);
	int16		nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
	BTPageOpaque opaque = BTPageGetOpaque(page);
	IndexTuple	itup;
	int			tupnatts;

	/*
	 * We cannot reliably test a deleted or half-dead page, since they have
	 * dummy high keys
	 */
	if (P_IGNORE(opaque))
		return true;

	Assert(offnum >= FirstOffsetNumber &&
		   offnum <= PageGetMaxOffsetNumber(page));

	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));
	tupnatts = BTreeTupleGetNAtts(itup, rel);

	/* !heapkeyspace indexes do not support deduplication */
	if (!heapkeyspace && BTreeTupleIsPosting(itup))
		return false;

	/* Posting list tuples should never have "pivot heap TID" bit set */
	if (BTreeTupleIsPosting(itup) &&
		(ItemPointerGetOffsetNumberNoCheck(&itup->t_tid) &
		 BT_PIVOT_HEAP_TID_ATTR) != 0)
		return false;

	/* INCLUDE indexes do not support deduplication */
	if (natts != nkeyatts && BTreeTupleIsPosting(itup))
		return false;

	if (P_ISLEAF(opaque))
	{
		if (offnum >= P_FIRSTDATAKEY(opaque))
		{
			/*
			 * Non-pivot tuple should never be explicitly marked as a pivot
			 * tuple
			 */
			if (BTreeTupleIsPivot(itup))
				return false;

			/*
			 * Leaf tuples that are not the page high key (non-pivot tuples)
			 * should never be truncated.  (Note that tupnatts must have been
			 * inferred, even with a posting list tuple, because only pivot
			 * tuples store tupnatts directly.)
			 */
			return tupnatts == natts;
		}
		else
		{
			/*
			 * Rightmost page doesn't contain a page high key, so tuple was
			 * checked above as ordinary leaf tuple
			 */
			Assert(!P_RIGHTMOST(opaque));

			/*
			 * !heapkeyspace high key tuple contains only key attributes. Note
			 * that tupnatts will only have been explicitly represented in
			 * !heapkeyspace indexes that happen to have non-key attributes.
			 */
			if (!heapkeyspace)
				return tupnatts == nkeyatts;

			/* Use generic heapkeyspace pivot tuple handling */
		}
	}
	else						/* !P_ISLEAF(opaque) */
	{
		if (offnum == P_FIRSTDATAKEY(opaque))
		{
			/*
			 * The first tuple on any internal page (possibly the first after
			 * its high key) is its negative infinity tuple.  Negative
			 * infinity tuples are always truncated to zero attributes.  They
			 * are a particular kind of pivot tuple.
			 */
			if (heapkeyspace)
				return tupnatts == 0;

			/*
			 * The number of attributes won't be explicitly represented if the
			 * negative infinity tuple was generated during a page split that
			 * occurred with a version of Postgres before v11.  There must be
			 * a problem when there is an explicit representation that is
			 * non-zero, or when there is no explicit representation and the
			 * tuple is evidently not a pre-pg_upgrade tuple.
			 *
			 * Prior to v11, downlinks always had P_HIKEY as their offset.
			 * Accept that as an alternative indication of a valid
			 * !heapkeyspace negative infinity tuple.
			 */
			return tupnatts == 0 ||
				ItemPointerGetOffsetNumber(&(itup->t_tid)) == P_HIKEY;
		}
		else
		{
			/*
			 * !heapkeyspace downlink tuple with separator key contains only
			 * key attributes.  Note that tupnatts will only have been
			 * explicitly represented in !heapkeyspace indexes that happen to
			 * have non-key attributes.
			 */
			if (!heapkeyspace)
				return tupnatts == nkeyatts;

			/* Use generic heapkeyspace pivot tuple handling */
		}
	}

	/* Handle heapkeyspace pivot tuples (excluding minus infinity items) */
	Assert(heapkeyspace);

	/*
	 * Explicit representation of the number of attributes is mandatory with
	 * heapkeyspace index pivot tuples, regardless of whether or not there are
	 * non-key attributes.
	 */
	if (!BTreeTupleIsPivot(itup))
		return false;

	/* Pivot tuple should not use posting list representation (redundant) */
	if (BTreeTupleIsPosting(itup))
		return false;

	/*
	 * Heap TID is a tiebreaker key attribute, so it cannot be untruncated
	 * when any other key attribute is truncated
	 */
	if (BTreeTupleGetHeapTID(itup) != NULL && tupnatts != nkeyatts)
		return false;

	/*
	 * Pivot tuple must have at least one untruncated key attribute (minus
	 * infinity pivot tuples are the only exception).  Pivot tuples can never
	 * represent that there is a value present for a key attribute that
	 * exceeds pg_index.indnkeyatts for the index.
	 */
	return tupnatts > 0 && tupnatts <= nkeyatts;
}

/*
 *
 *  _dmbt_check_third_page() -- check whether tuple fits on a btree page at all.
 *
 * We actually need to be able to fit three items on every page, so restrict
 * any one item to 1/3 the per-page available space.  Note that itemsz should
 * not include the ItemId overhead.
 *
 * It might be useful to apply TOAST methods rather than throw an error here.
 * Using out of line storage would break assumptions made by suffix truncation
 * and by contrib/amcheck, though.
 */
void
_dmbt_check_third_page(Relation rel, Relation heap, bool needheaptidspace,
					 Page page, IndexTuple newtup)
{
	Size		itemsz;
	BTPageOpaque opaque;

	itemsz = MAXALIGN(IndexTupleSize(newtup));

	/* Double check item size against limit */
	if (itemsz <= BTMaxItemSize)
		return;

	/*
	 * Tuple is probably too large to fit on page, but it's possible that the
	 * index uses version 2 or version 3, or that page is an internal page, in
	 * which case a slightly higher limit applies.
	 */
	if (!needheaptidspace && itemsz <= BTMaxItemSizeNoHeapTid)
		return;

	/*
	 * Internal page insertions cannot fail here, because that would mean that
	 * an earlier leaf level insertion that should have failed didn't
	 */
	opaque = BTPageGetOpaque(page);
	if (!P_ISLEAF(opaque))
		elog(ERROR, "cannot insert oversized tuple of size %zu on internal page of index \"%s\"",
			 itemsz, RelationGetRelationName(rel));

	ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("index row size %zu exceeds btree version %u maximum %zu for index \"%s\"",
					itemsz,
					needheaptidspace ? BTREE_VERSION : BTREE_NOVAC_VERSION,
					needheaptidspace ? BTMaxItemSize : BTMaxItemSizeNoHeapTid,
					RelationGetRelationName(rel)),
			 errdetail("Index row references tuple (%u,%u) in relation \"%s\".",
					   ItemPointerGetBlockNumber(BTreeTupleGetHeapTID(newtup)),
					   DmbtTidGetOffsetNumber(BTreeTupleGetHeapTID(newtup)),
					   RelationGetRelationName(heap)),
			 errhint("Values larger than 1/3 of a buffer page cannot be indexed.\n"
					 "Consider a function index of an MD5 hash of the value, "
					 "or use full text indexing."),
			 errtableconstraint(heap, RelationGetRelationName(rel))));
}

/*
 * dmbtree: do all key columns' opclasses have "equality implies image
 * equality"?  The part of _dmbt_allequalimage() that INCLUDE columns do not
 * affect, which is what in-place updates of key columns need
 * (dmbtree_dm_capable()).
 */
bool
_dmbt_keys_equalimage(Relation rel)
{
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(rel); i++)
	{
		Oid			opfamily = rel->rd_opfamily[i];
		Oid			opcintype = rel->rd_opcintype[i];
		Oid			collation = rel->rd_indcollation[i];
		Oid			equalimageproc;

		equalimageproc = get_opfamily_proc(opfamily, opcintype, opcintype,
										   BTEQUALIMAGE_PROC);

		/*
		 * If there is no BTEQUALIMAGE_PROC then deduplication is assumed to
		 * be unsafe.  Otherwise, actually call proc and see what it says.
		 */
		if (!OidIsValid(equalimageproc) ||
			!DatumGetBool(OidFunctionCall1Coll(equalimageproc, collation,
											   ObjectIdGetDatum(opcintype))))
			return false;
	}
	return true;
}

/*
 * dmbtree: refuse a pattern operator class (text_pattern_ops,
 * varchar_pattern_ops, bpchar_pattern_ops) with a nondeterministic
 * collation, as index_create() does for btree's.  These classes compare
 * bytes, but their equality operator is texteq, which follows the
 * collation, so an index scan would miss rows that are equal but differ in
 * bytes.  index_create() checks btree's OIDs; dmbtree's copies have the
 * same names in pg_catalog.  Called from ambuild, which every index build
 * goes through.
 */
void
_dmbt_check_collations(Relation rel)
{
	oidvector  *indclass;

	indclass = (oidvector *)
		DatumGetPointer(SysCacheGetAttrNotNull(INDEXRELID, rel->rd_indextuple,
											   Anum_pg_index_indclass));
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(rel); i++)
	{
		Oid			collation = rel->rd_indcollation[i];
		HeapTuple	tp;
		Form_pg_opclass form;

		if (!OidIsValid(collation) || get_collation_isdeterministic(collation))
			continue;
		tp = SearchSysCache1(CLAOID, ObjectIdGetDatum(indclass->values[i]));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for operator class %u",
				 indclass->values[i]);
		form = (Form_pg_opclass) GETSTRUCT(tp);
		if (form->opcnamespace == PG_CATALOG_NAMESPACE &&
			(strcmp(NameStr(form->opcname), "text_pattern_ops") == 0 ||
			 strcmp(NameStr(form->opcname), "varchar_pattern_ops") == 0 ||
			 strcmp(NameStr(form->opcname), "bpchar_pattern_ops") == 0))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("nondeterministic collations are not supported for operator class \"%s\"",
							NameStr(form->opcname))));
		ReleaseSysCache(tp);
	}
}

/*
 * dmbtree: keep snapshots older than this transaction off a rebuilt index
 * (REINDEX, VACUUM FULL, CLUSTER) whose build found rows updated in place
 * that some snapshot may not see yet (ii_BrokenHotChain).  index_build()
 * sets indcheckxmin for a new index only: an existing btree index has the
 * same key in every version of a row.  A dmbtree index does not, as
 * in-place updates change its key columns, and the rebuilt index has the
 * newest version's key only.  The pg_index row is updated even if the flag
 * is set, since its xmin is the horizon; the table's relcache invalidation
 * makes cached plans that use the index be planned again.
 */
void
_dmbt_set_checkxmin(Relation heap, Relation index)
{
	Relation	pg_index = table_open(IndexRelationId, RowExclusiveLock);
	HeapTuple	tup;

	tup = SearchSysCacheCopy1(INDEXRELID,
							  ObjectIdGetDatum(RelationGetRelid(index)));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for index %u",
			 RelationGetRelid(index));
	((Form_pg_index) GETSTRUCT(tup))->indcheckxmin = true;
	CatalogTupleUpdate(pg_index, &tup->t_self, tup);
	heap_freetuple(tup);
	table_close(pg_index, RowExclusiveLock);

	CacheInvalidateRelcache(heap);
}

/*
 * Are all attributes in rel "equality is image equality" attributes?
 *
 * We use each attribute's BTEQUALIMAGE_PROC opclass procedure.  If any
 * opclass either lacks a BTEQUALIMAGE_PROC procedure or returns false, we
 * return false; otherwise we return true.
 *
 * Returned boolean value is stored in index metapage during index builds.
 * Deduplication can only be used when we return true.
 */
bool
_dmbt_allequalimage(Relation rel, bool debugmessage)
{
	bool		allequalimage;

	/* INCLUDE indexes can never support deduplication */
	if (IndexRelationGetNumberOfAttributes(rel) !=
		IndexRelationGetNumberOfKeyAttributes(rel))
		return false;

	allequalimage = _dmbt_keys_equalimage(rel);

	if (debugmessage)
	{
		if (allequalimage)
			elog(DEBUG1, "index \"%s\" can safely use deduplication",
				 RelationGetRelationName(rel));
		else
			elog(DEBUG1, "index \"%s\" cannot use deduplication",
				 RelationGetRelationName(rel));
	}

	return allequalimage;
}
