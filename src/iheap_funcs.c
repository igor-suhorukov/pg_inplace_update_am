/*-------------------------------------------------------------------------
 *
 * iheap_funcs.c
 *	  SQL-callable inspection functions for iheap relations (used by the
 *	  tests and for diagnostics; superuser only, see the extension script).
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"

#include "iheap.h"

PG_FUNCTION_INFO_V1(iheap_metapage_info);
PG_FUNCTION_INFO_V1(iheap_page_items);
PG_FUNCTION_INFO_V1(iheap_undo_page_records);
PG_FUNCTION_INFO_V1(iheap_relation_stats);
PG_FUNCTION_INFO_V1(iheap_discard_undo);

/* open relid with lockmode, insisting that it is an iheap relation */
Relation
iheap_open_relation(Oid relid, LOCKMODE lockmode)
{
	Relation	rel = relation_open(relid, lockmode);

	if (!RELKIND_HAS_TABLE_AM(rel->rd_rel->relkind) ||
		rel->rd_tableam == NULL ||
		rel->rd_tableam->slot_callbacks(rel) != &TTSOpsIHeapTuple)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an iheap relation",
						RelationGetRelationName(rel))));
	return rel;
}

/* copy of a block, read under a share lock */
static void
read_page_copy(Relation rel, BlockNumber blkno, char *dst)
{
	Buffer		buf;

	if (blkno >= RelationGetNumberOfBlocks(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("block number %u is out of range for relation \"%s\"",
						blkno, RelationGetRelationName(rel))));

	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, NULL);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	memcpy(dst, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
}

static const char *
page_type_name(Page page)
{
	switch (IHeapPageGetType(page))
	{
		case IHEAP_PAGE_META:
			return "meta";
		case IHEAP_PAGE_DATA:
			return "data";
		case IHEAP_PAGE_UNDO:
			return "undo";
		default:
			return PageIsNew(page) ? "new" : "unknown";
	}
}

/*
 * iheap_metapage_info(rel) -> (discard_ptr, next_lpage, nextents,
 *								live_undo_pages, free_extent_pages,
 *								undo_peak_pages)
 */
Datum
iheap_metapage_info(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel = iheap_open_relation(relid, AccessShareLock);
	PGIOAlignedBlock page;
	IHeapMetaPageData *meta;
	TupleDesc	tupdesc;
	Datum		values[6];
	bool		nulls[6] = {0};
	int64		free_pages = 0;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	read_page_copy(rel, IHEAP_METAPAGE_BLKNO, page.data);
	if (IHeapPageGetType(page.data) != IHEAP_PAGE_META)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block 0 of \"%s\" is not an iheap metapage",
						RelationGetRelationName(rel))));
	meta = IHeapPageGetMeta(page.data);
	for (uint32 i = 0; i < meta->im_nextents; i++)
	{
		if (meta->im_extents[i].ie_lstart == IHEAP_EXTENT_FREE)
			free_pages += meta->im_extents[i].ie_npages;
	}

	values[0] = Int64GetDatum((int64) meta->im_discard);
	values[1] = Int64GetDatum((int64) meta->im_next_lpage);
	values[2] = Int32GetDatum((int32) meta->im_nextents);
	values[3] = Int64GetDatum((int64) iheap_meta_live_pages(meta));
	values[4] = Int64GetDatum(free_pages);
	values[5] = Int64GetDatum((int64) meta->im_undo_peak);

	relation_close(rel, AccessShareLock);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

static Datum
tuple_flags_array(uint16 infomask)
{
	static const struct
	{
		uint16		flag;
		const char *name;
	}			names[] = {
		{IHT_HASNULL, "HASNULL"},
		{IHT_HASVARWIDTH, "HASVARWIDTH"},
		{IHT_HASEXTERNAL, "HASEXTERNAL"},
		{IHT_ALIGNDATA, "ALIGNDATA"},
		{IHT_XID_COMMITTED, "XID_COMMITTED"},
		{IHT_XID_ABORTED, "XID_ABORTED"},
		{IHT_DELETED, "DELETED"},
		{IHT_MOVED, "MOVED"},
		{IHT_LOCKED, "LOCKED"},
		{IHT_INPLACE, "INPLACE"},
		{IHT_SPECULATIVE, "SPECULATIVE"},
		{IHT_STUB, "STUB"},
		{IHT_KEYS_UPDATED, "KEYS_UPDATED"},
		{IHT_MOVED_PART, "MOVED_PART"},
	};
	Datum		elems[lengthof(names)];
	int			n = 0;

	for (int i = 0; i < lengthof(names); i++)
	{
		if (infomask & names[i].flag)
			elems[n++] = CStringGetTextDatum(names[i].name);
	}
	return PointerGetDatum(construct_array_builtin(elems, n, TEXTOID));
}

/*
 * iheap_page_items(rel, blkno) -> setof (lp, lp_flags, lp_off, lp_len, xid,
 *		urec, frozen, natts, hoff, flags)
 */
Datum
iheap_page_items(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		blkno = PG_GETARG_INT64(1);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	PGIOAlignedBlock page;
	OffsetNumber maxoff;
	UndoRecPtr	discard;

	InitMaterializedSRF(fcinfo, 0);

	if (blkno < 0 || blkno > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid block number " INT64_FORMAT, blkno)));

	rel = iheap_open_relation(relid, AccessShareLock);
	read_page_copy(rel, (BlockNumber) blkno, page.data);
	discard = iheap_discard_ptr(rel);

	if (!IHeapPageIsData(page.data))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("block " INT64_FORMAT " of \"%s\" is a %s page, not a data page",
						blkno, RelationGetRelationName(rel),
						page_type_name(page.data))));

	maxoff = PageGetMaxOffsetNumber(page.data);
	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		lp = PageGetItemId(page.data, off);
		Datum		values[10];
		bool		nulls[10] = {0};

		values[0] = Int32GetDatum(off);
		values[1] = Int32GetDatum(ItemIdGetFlags(lp));
		values[2] = Int32GetDatum(ItemIdGetOffset(lp));
		values[3] = Int32GetDatum(ItemIdGetLength(lp));

		if (ItemIdIsNormal(lp) && ItemIdGetLength(lp) >= IHEAP_TUPHDR_SIZE &&
			ItemIdGetOffset(lp) + ItemIdGetLength(lp) <= BLCKSZ)
		{
			IHeapHdr	h;

			ih_hdr_read((char *) PageGetItem(page.data, lp), &h);
			values[4] = TransactionIdGetDatum(h.xid);
			values[5] = Int64GetDatum((int64) h.urec);
			values[6] = BoolGetDatum(h.urec < discard);
			values[7] = Int32GetDatum(h.infomask2 & IHT_NATTS_MASK);
			values[8] = Int32GetDatum(h.hoff);
			values[9] = tuple_flags_array(h.infomask);
		}
		else
		{
			for (int i = 4; i < 10; i++)
				nulls[i] = true;
		}
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	relation_close(rel, AccessShareLock);
	return (Datum) 0;
}

static const char *
undo_type_name(uint8 type)
{
	switch (type)
	{
		case IHU_INSERT:
			return "INSERT";
		case IHU_MULTI_INSERT:
			return "MULTI_INSERT";
		case IHU_DELETE:
			return "DELETE";
		case IHU_UPDATE:
			return "UPDATE";
		case IHU_MOVE:
			return "MOVE";
		case IHU_LOCK:
			return "LOCK";
		default:
			return "UNKNOWN";
	}
}

/*
 * iheap_undo_page_records(rel, blkno) -> setof (urp, type, flags, xid, cid,
 *		target_block, target_offset, len, xact_prev)
 */
Datum
iheap_undo_page_records(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		blkno = PG_GETARG_INT64(1);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	PGIOAlignedBlock page;
	IHeapUndoPageHeaderData *uph;
	uint32		off;
	uint32		lower;

	InitMaterializedSRF(fcinfo, 0);

	if (blkno < 0 || blkno > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid block number " INT64_FORMAT, blkno)));

	rel = iheap_open_relation(relid, AccessShareLock);
	read_page_copy(rel, (BlockNumber) blkno, page.data);

	if (IHeapPageGetType(page.data) != IHEAP_PAGE_UNDO)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("block " INT64_FORMAT " of \"%s\" is a %s page, not an undo page",
						blkno, RelationGetRelationName(rel),
						page_type_name(page.data))));

	uph = IHeapUndoPageGetHeader(page.data);
	lower = ((PageHeader) page.data)->pd_lower;
	off = IHEAP_UNDO_DATA_START;
	while (off < lower)
	{
		IHeapUndoRecHeader hdr;
		const char *payload;
		uint32		payload_len;
		Datum		values[9];
		bool		nulls[9] = {0};

		if (!iheap_undo_read_record(page.data, off, lower, &hdr, &payload,
									&payload_len))
			break;

		values[0] = Int64GetDatum((int64) MakeUndoRecPtr(uph->uph_lpage, off));
		values[1] = CStringGetTextDatum(undo_type_name(hdr.ur_type));
		values[2] = Int32GetDatum(hdr.ur_flags);
		values[3] = TransactionIdGetDatum(hdr.ur_xid);
		values[4] = Int64GetDatum((int64) hdr.ur_cid);
		values[5] = Int64GetDatum((int64) hdr.ur_blkno);
		values[6] = Int32GetDatum(hdr.ur_offnum);
		values[7] = Int32GetDatum(hdr.ur_len);
		values[8] = Int64GetDatum((int64) hdr.ur_xact_prev);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

		off += hdr.ur_len;
	}

	relation_close(rel, AccessShareLock);
	return (Datum) 0;
}

/*
 * iheap_relation_stats(rel) -> (nblocks, data_pages, undo_pages, other_pages,
 *		live_items, frozen_items, deleted_items, stub_items, dead_items,
 *		unused_items, free_bytes)
 *
 * live_items: items whose current version is a row; frozen_items: those of
 * them whose undo has been discarded; deleted_items: items whose current
 * version is a deletion (or a move away) that still have their body;
 * stub_items: the same, body reclaimed.
 *
 * Reads every block; meant for tests and diagnostics.
 */
Datum
iheap_relation_stats(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel = iheap_open_relation(relid, AccessShareLock);
	TupleDesc	tupdesc;
	BlockNumber nblocks;
	int64		data_pages = 0,
				undo_pages = 0,
				other_pages = 0,
				live = 0,
				frozen = 0,
				deleted = 0,
				stubs = 0,
				dead = 0,
				unused = 0,
				free_bytes = 0;
	Datum		values[11];
	bool		nulls[11] = {0};
	BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);
	UndoRecPtr	discard;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	nblocks = RelationGetNumberOfBlocks(rel);
	discard = iheap_discard_ptr(rel);
	for (BlockNumber blk = 0; blk < nblocks; blk++)
	{
		Buffer		buf;
		Page		page;

		CHECK_FOR_INTERRUPTS();
		buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_NORMAL, bstrategy);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);

		switch (IHeapPageGetType(page))
		{
			case IHEAP_PAGE_DATA:
				{
					OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

					data_pages++;
					free_bytes += iheap_page_free_space(page);
					for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
					{
						ItemId		lp = PageGetItemId(page, off);

						if (ItemIdIsNormal(lp))
						{
							IHeapHdr	h;

							ih_hdr_read((char *) PageGetItem(page, lp), &h);
							if (h.infomask & IHT_STUB)
								stubs++;
							else if (h.infomask & (IHT_DELETED | IHT_MOVED))
								deleted++;
							else
							{
								live++;
								if (h.urec < discard)
									frozen++;
							}
						}
						else if (ItemIdIsDead(lp))
							dead++;
						else
							unused++;
					}
				}
				break;
			case IHEAP_PAGE_UNDO:
				undo_pages++;
				break;
			default:
				other_pages++;
				break;
		}
		UnlockReleaseBuffer(buf);
	}
	FreeAccessStrategy(bstrategy);

	values[0] = Int64GetDatum((int64) nblocks);
	values[1] = Int64GetDatum(data_pages);
	values[2] = Int64GetDatum(undo_pages);
	values[3] = Int64GetDatum(other_pages);
	values[4] = Int64GetDatum(live);
	values[5] = Int64GetDatum(frozen);
	values[6] = Int64GetDatum(deleted);
	values[7] = Int64GetDatum(stubs);
	values[8] = Int64GetDatum(dead);
	values[9] = Int64GetDatum(unused);
	values[10] = Int64GetDatum(free_bytes);

	relation_close(rel, AccessShareLock);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* iheap_discard(rel) -> logical undo pages discarded */
Datum
iheap_discard_undo(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;
	int64		n;

	rel = relation_open(relid, RowExclusiveLock);
	if (!RELKIND_HAS_TABLE_AM(rel->rd_rel->relkind) ||
		rel->rd_tableam->slot_callbacks(rel) != &TTSOpsIHeapTuple)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an iheap relation",
						RelationGetRelationName(rel))));
	n = iheap_discard(rel, true);
	relation_close(rel, RowExclusiveLock);

	PG_RETURN_INT64(n);
}
