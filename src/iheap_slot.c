/*-------------------------------------------------------------------------
 *
 * iheap_slot.c
 *	  TupleTableSlot implementation for iheap tuples.
 *
 * The slot never points into a shared buffer: tuples are copied out while
 * the page's content lock is held, either into a scan's private page copy
 * (the slot then references that copy and the scan keeps it valid until the
 * next tuple is fetched) or into memory the slot owns.
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "executor/tuptable.h"
#include "utils/memutils.h"

#include "iheap.h"

static void
tts_iheap_init(TupleTableSlot *slot)
{
	IHeapTupleTableSlot *islot = (IHeapTupleTableSlot *) slot;

	islot->tuple = NULL;
	islot->mem = NULL;
	islot->tuplen = 0;
	islot->off = 0;
	islot->xmin = InvalidTransactionId;
	islot->xmax = InvalidTransactionId;
	islot->cmin = InvalidCommandId;
}

static void
tts_iheap_release(TupleTableSlot *slot)
{
}

static void
tts_iheap_clear(TupleTableSlot *slot)
{
	IHeapTupleTableSlot *islot = (IHeapTupleTableSlot *) slot;

	if (TTS_SHOULDFREE(slot))
	{
		if (islot->mem)
			pfree(islot->mem);
		slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;
	}

	islot->tuple = NULL;
	islot->mem = NULL;
	islot->tuplen = 0;
	islot->off = 0;
	islot->xmin = InvalidTransactionId;
	islot->xmax = InvalidTransactionId;
	islot->cmin = InvalidCommandId;

	slot->tts_nvalid = 0;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	ItemPointerSetInvalid(&slot->tts_tid);
}

static void
tts_iheap_getsomeattrs(TupleTableSlot *slot, int natts)
{
	IHeapTupleTableSlot *islot = (IHeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	if (islot->tuple == NULL)
	{
		/* virtual contents: all values are already there */
		Assert(slot->tts_nvalid >= natts);
		return;
	}

	ih_deform_into_slot(slot, islot->tuple, &islot->off, natts);
}

static Datum
tts_iheap_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	IHeapTupleTableSlot *islot = (IHeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));
	*isnull = false;

	switch (attnum)
	{
		case MinTransactionIdAttributeNumber:
			return TransactionIdGetDatum(islot->xmin);
		case MaxTransactionIdAttributeNumber:
			return TransactionIdGetDatum(islot->xmax);
		case MinCommandIdAttributeNumber:
		case MaxCommandIdAttributeNumber:
			return CommandIdGetDatum(islot->cmin == InvalidCommandId ?
									 FirstCommandId : islot->cmin);
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("system column %d is not supported by the iheap access method",
							attnum)));
	}
	return (Datum) 0;			/* keep compiler quiet */
}

static bool
tts_iheap_is_current_xact_tuple(TupleTableSlot *slot)
{
	IHeapTupleTableSlot *islot = (IHeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	if (!TransactionIdIsValid(islot->xmin))
		return false;
	return TransactionIdIsCurrentTransactionId(islot->xmin);
}

static void
tts_iheap_materialize(TupleTableSlot *slot)
{
	IHeapTupleTableSlot *islot = (IHeapTupleTableSlot *) slot;
	MemoryContext oldcxt;

	Assert(!TTS_EMPTY(slot));

	if (TTS_SHOULDFREE(slot))
		return;

	oldcxt = MemoryContextSwitchTo(slot->tts_mcxt);

	if (islot->tuple == NULL)
	{
		/* virtual contents (values set directly): form a tuple */
		uint32		len;
		char	   *mem;

		islot->tuple = ih_form_tuple(slot->tts_tupleDescriptor,
									 slot->tts_values, slot->tts_isnull,
									 &len, &mem);
		islot->tuplen = len;
		islot->mem = mem;
	}
	else
	{
		char	   *mem;

		islot->tuple = ih_copy_tuple_aligned(slot->tts_mcxt, islot->tuple,
											 islot->tuplen, &mem);
		islot->mem = mem;
	}

	/* values may have pointed into the old storage: deform again */
	slot->tts_nvalid = 0;
	islot->off = 0;
	slot->tts_flags |= TTS_FLAG_SHOULDFREE;

	MemoryContextSwitchTo(oldcxt);
}

static void
tts_iheap_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	IHeapTupleTableSlot *dst = (IHeapTupleTableSlot *) dstslot;

	if (TTS_IS_IHEAPTUPLE(srcslot) &&
		((IHeapTupleTableSlot *) srcslot)->tuple != NULL &&
		dstslot->tts_tupleDescriptor->natts == srcslot->tts_tupleDescriptor->natts)
	{
		IHeapTupleTableSlot *src = (IHeapTupleTableSlot *) srcslot;

		iheap_store_tuple(dstslot, src->tuple, src->tuplen, true,
						  src->xmin, src->xmax, src->cmin);
	}
	else
	{
		MemoryContext oldcxt;
		uint32		len;
		char	   *mem;
		char	   *tup;

		slot_getallattrs(srcslot);
		oldcxt = MemoryContextSwitchTo(dstslot->tts_mcxt);
		tup = ih_form_tuple(dstslot->tts_tupleDescriptor,
							srcslot->tts_values, srcslot->tts_isnull,
							&len, &mem);
		MemoryContextSwitchTo(oldcxt);

		ExecClearTuple(dstslot);
		dst->tuple = tup;
		dst->mem = mem;
		dst->tuplen = len;
		dst->off = 0;
		dstslot->tts_nvalid = 0;
		dstslot->tts_flags &= ~TTS_FLAG_EMPTY;
		dstslot->tts_flags |= TTS_FLAG_SHOULDFREE;
	}
	dstslot->tts_tid = srcslot->tts_tid;
}

static HeapTuple
tts_iheap_copy_heap_tuple(TupleTableSlot *slot)
{
	IHeapTupleTableSlot *islot = (IHeapTupleTableSlot *) slot;
	HeapTuple	tuple;

	Assert(!TTS_EMPTY(slot));

	slot_getallattrs(slot);
	tuple = heap_form_tuple(slot->tts_tupleDescriptor,
							slot->tts_values, slot->tts_isnull);
	tuple->t_self = slot->tts_tid;
	tuple->t_tableOid = slot->tts_tableOid;
	if (TransactionIdIsValid(islot->xmin))
		HeapTupleHeaderSetXmin(tuple->t_data, islot->xmin);
	return tuple;
}

static MinimalTuple
tts_iheap_copy_minimal_tuple(TupleTableSlot *slot, Size extra)
{
	Assert(!TTS_EMPTY(slot));

	slot_getallattrs(slot);
	return heap_form_minimal_tuple(slot->tts_tupleDescriptor,
								   slot->tts_values, slot->tts_isnull,
								   extra);
}

const TupleTableSlotOps TTSOpsIHeapTuple = {
	.base_slot_size = sizeof(IHeapTupleTableSlot),
	.init = tts_iheap_init,
	.release = tts_iheap_release,
	.clear = tts_iheap_clear,
	.getsomeattrs = tts_iheap_getsomeattrs,
	.getsysattr = tts_iheap_getsysattr,
	.is_current_xact_tuple = tts_iheap_is_current_xact_tuple,
	.materialize = tts_iheap_materialize,
	.copyslot = tts_iheap_copyslot,
	.get_heap_tuple = NULL,
	.get_minimal_tuple = NULL,
	.copy_heap_tuple = tts_iheap_copy_heap_tuple,
	.copy_minimal_tuple = tts_iheap_copy_minimal_tuple
};

/*
 * Store a tuple in an iheap slot (or, for a slot of another type, its
 * deformed values).  With copy = false the caller guarantees "tuple" stays
 * valid until the slot is cleared or the next tuple is stored, and that it
 * is suitably aligned.
 */
void
iheap_store_tuple(TupleTableSlot *slot, char *tuple, uint32 len, bool copy,
				  TransactionId xmin, TransactionId xmax, CommandId cmin)
{
	IHeapTupleTableSlot *islot;

	if (!TTS_IS_IHEAPTUPLE(slot))
	{
		ExecClearTuple(slot);
		ih_deform_tuple(slot->tts_tupleDescriptor, tuple,
						slot->tts_values, slot->tts_isnull);
		ExecStoreVirtualTuple(slot);
		/* the values point into "tuple": make the slot self-contained */
		ExecMaterializeSlot(slot);
		return;
	}

	islot = (IHeapTupleTableSlot *) slot;
	tts_iheap_clear(slot);

	if (copy)
	{
		char	   *mem;

		islot->tuple = ih_copy_tuple_aligned(slot->tts_mcxt, tuple, len, &mem);
		islot->mem = mem;
		slot->tts_flags |= TTS_FLAG_SHOULDFREE;
	}
	else
		islot->tuple = tuple;

	islot->tuplen = len;
	islot->xmin = xmin;
	islot->xmax = xmax;
	islot->cmin = cmin;
	slot->tts_flags &= ~TTS_FLAG_EMPTY;
	slot->tts_nvalid = 0;
}

void
iheap_store_version(TupleTableSlot *slot, IHeapVersion *v, bool copy)
{
	iheap_store_tuple(slot, v->tuple, v->len, copy, v->xmin, v->xmax, v->cmin);
}
