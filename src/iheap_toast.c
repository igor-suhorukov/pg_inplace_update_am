/*-------------------------------------------------------------------------
 *
 * iheap_toast.c
 *	  Out-of-line storage of large values for iheap.
 *
 * Large values go to a regular heap TOAST table through toast_helper.c
 * (DESIGN.md A6, §21 D14), following heap_toast_insert_or_update() but with
 * iheap's tuple size computation.  The toast table's own MVCC keeps old
 * values visible to old snapshots; undo restores pointers to them on
 * rollback.
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heaptoast.h"
#include "access/toast_helper.h"
#include "catalog/pg_class.h"
#include "utils/rel.h"
#include "varatt.h"

#include "iheap.h"

static Size
ih_header_size(TupleDesc desc, const bool *isnull, bool hasnulls_flag)
{
	return IHEAP_TUPHDR_SIZE + (hasnulls_flag ? BITMAPLEN(desc->natts) : 0);
}

static Size
ih_data_size(TupleDesc desc, const Datum *values, const bool *isnull)
{
	bool		needs_align;

	return ih_compute_data_size(desc, values, isnull, &needs_align);
}

/*
 * Does a tuple with these values need the toaster?  old_has_external:
 * the tuple being replaced has out-of-line values (they may need deleting).
 */
bool
iheap_tuple_needs_toast(Relation rel, TupleDesc desc, const Datum *values,
						const bool *isnull, bool old_has_external)
{
	bool		hasnull = false;

	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_MATVIEW)
		return false;

	if (old_has_external)
		return true;

	for (int i = 0; i < desc->natts; i++)
	{
		if (isnull[i])
		{
			hasnull = true;
			continue;
		}
		if (TupleDescCompactAttr(desc, i)->attlen == -1 &&
			VARATT_IS_EXTERNAL(DatumGetPointer(values[i])))
			return true;
	}

	return ih_header_size(desc, isnull, hasnull) +
		ih_data_size(desc, values, isnull) > TOAST_TUPLE_THRESHOLD;
}

/*
 * Compress and/or move values out of line until the tuple fits the target.
 * Modifies values[] in place (new datums are palloc'd).  oldvalues/oldisnull
 * describe the version being replaced (NULL for inserts): values shared
 * with it are kept, replaced ones are deleted by iheap_toast_end().
 */
void
iheap_toast_begin(IHeapToastState *ts, Relation rel, TupleDesc desc,
				  Datum *values, bool *isnull, Datum *oldvalues,
				  bool *oldisnull, uint32 options)
{
	ToastTupleContext *ttc = &ts->ttc;
	Size		hoff;
	Size		maxDataLen;

	Assert(desc->natts <= MaxHeapAttributeNumber);

	ttc->ttc_rel = rel;
	ttc->ttc_values = values;
	ttc->ttc_isnull = isnull;
	ttc->ttc_oldvalues = oldvalues;
	ttc->ttc_oldisnull = oldisnull;
	ttc->ttc_attr = ts->attr;
	toast_tuple_init(ttc);
	ts->active = true;

	hoff = ih_header_size(desc, isnull, (ttc->ttc_flags & TOAST_HAS_NULLS) != 0);
	maxDataLen = RelationGetToastTupleTarget(rel, TOAST_TUPLE_TARGET) - hoff;

	/* 1: compress EXTENDED, push very large EXTENDED/EXTERNAL out */
	while (ih_data_size(desc, values, isnull) > maxDataLen)
	{
		int			biggest = toast_tuple_find_biggest_attribute(ttc, true, false);

		if (biggest < 0)
			break;
		if (TupleDescAttr(desc, biggest)->attstorage == TYPSTORAGE_EXTENDED)
			toast_tuple_try_compression(ttc, biggest);
		else
			ts->attr[biggest].tai_colflags |= TOASTCOL_INCOMPRESSIBLE;

		if (ts->attr[biggest].tai_size > maxDataLen &&
			rel->rd_rel->reltoastrelid != InvalidOid)
			toast_tuple_externalize(ttc, biggest, options);
	}

	/* 2: push EXTENDED/EXTERNAL out */
	while (ih_data_size(desc, values, isnull) > maxDataLen &&
		   rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int			biggest = toast_tuple_find_biggest_attribute(ttc, false, false);

		if (biggest < 0)
			break;
		toast_tuple_externalize(ttc, biggest, options);
	}

	/* 3: compress MAIN */
	while (ih_data_size(desc, values, isnull) > maxDataLen)
	{
		int			biggest = toast_tuple_find_biggest_attribute(ttc, true, true);

		if (biggest < 0)
			break;
		toast_tuple_try_compression(ttc, biggest);
	}

	/* 4: push MAIN out, with a larger target */
	maxDataLen = TOAST_TUPLE_TARGET_MAIN - hoff;
	while (ih_data_size(desc, values, isnull) > maxDataLen &&
		   rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int			biggest = toast_tuple_find_biggest_attribute(ttc, false, true);

		if (biggest < 0)
			break;
		toast_tuple_externalize(ttc, biggest, options);
	}
}

/* free temporary values and delete replaced out-of-line values */
void
iheap_toast_end(IHeapToastState *ts)
{
	if (ts->active)
	{
		toast_tuple_cleanup(&ts->ttc);
		ts->active = false;
	}
}

/* delete the out-of-line values of a tuple (row deleted) */
void
iheap_toast_delete_values(Relation rel, TupleDesc desc, const char *tuple,
						  bool is_speculative)
{
	Datum		values[MaxHeapAttributeNumber];
	bool		isnull[MaxHeapAttributeNumber];

	if ((ih_hdr_infomask(tuple) & IHT_HASEXTERNAL) == 0)
		return;
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_MATVIEW)
		return;

	ih_deform_tuple(desc, tuple, values, isnull);
	toast_delete_external(rel, values, isnull, is_speculative);
}
