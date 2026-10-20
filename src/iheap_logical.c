/*-------------------------------------------------------------------------
 *
 * iheap_logical.c
 *	  Logical decoding of iheap's WAL records (DESIGN.md §9.5, §21 D38).
 *
 * Output plugins read the rows of a change as heap tuples, deformed with
 * the relation's descriptor, which only the replay of a transaction looks
 * up (under its historic snapshot).  rm_decode runs before that, with
 * neither a descriptor nor catalog access, and iheap's tuples are not laid
 * out as heap's (§5.4, §21 D11).  So a record written while logical
 * decoding needs it (RelationIsLogicallyLogged) carries, in its main data
 * after the record's own struct, the *layout* of the relation's attributes
 * (length, alignment, by-value; one byte each, usually), from which the
 * decoder rebuilds a heap tuple out of the iheap tuple the record carries
 * anyway; the old row's replica identity, which iheap's records do not
 * otherwise carry (undo holds a delta), is formed in heap's layout already
 * by the writer, as heap logs it (ExtractReplicaIdentity()).  Records
 * written with the layout keep their tuple data in WAL even when the page
 * gets a full-page image (REGBUF_KEEP_DATA), and in-place updates log the
 * whole new tuple instead of the changed middle part.
 *
 * Main data of a decodable record, after the record's struct:
 *
 *		uint8	lflags		XLIL_*
 *		uint16	natts		[XLIL_LAYOUT] attributes described
 *		layout				[XLIL_LAYOUT] one byte per attribute (and two
 *							more for an unusual fixed length)
 *		uint32	len			[XLIL_OLD_*] the old row's identity: the heap
 *		header, data		tuple's infomask2, infomask, hoff (5 bytes),
 *							then its bitmap and data (len - 23 bytes)
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/heapam_xlog.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/reorderbuffer.h"
#include "replication/snapbuild.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "varatt.h"

#include "iheap.h"
#include "iheap_xlog.h"

/* the layout byte of an attribute */
#define IHLA_ALIGN_MASK		0x03	/* 'c', 's', 'i', 'd' */
#define IHLA_BYVAL			0x04
#define IHLA_PACKABLE		0x08	/* attstorage is not plain */
#define IHLA_LEN_SHIFT		4		/* length code: */
#define IHLA_LEN_VARLENA	0		/* -1 */
#define IHLA_LEN_CSTRING	1		/* -2 */
#define IHLA_LEN_1			2
#define IHLA_LEN_2			3
#define IHLA_LEN_4			4
#define IHLA_LEN_8			5
#define IHLA_LEN_OTHER		6		/* uint16 length follows */

static const char ihla_align_char[4] = {
	TYPALIGN_CHAR, TYPALIGN_SHORT, TYPALIGN_INT, TYPALIGN_DOUBLE
};

/* ------------------------------------------------------------------------
 * Writing
 * ------------------------------------------------------------------------
 */

/* the layout of desc's attributes; returns its length (at most 3 per attribute) */
static Size
layout_encode(TupleDesc desc, char *buf)
{
	char	   *p = buf;
	uint16		natts = (uint16) desc->natts;

	memcpy(p, &natts, sizeof(uint16));
	p += sizeof(uint16);
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		uint8		b = 0;
		int			code;

		switch (att->attalign)
		{
			case TYPALIGN_SHORT:
				b |= 1;
				break;
			case TYPALIGN_INT:
				b |= 2;
				break;
			case TYPALIGN_DOUBLE:
				b |= 3;
				break;
			default:
				break;
		}
		if (att->attbyval)
			b |= IHLA_BYVAL;
		if (att->attstorage != TYPSTORAGE_PLAIN)
			b |= IHLA_PACKABLE;
		switch (att->attlen)
		{
			case -1:
				code = IHLA_LEN_VARLENA;
				break;
			case -2:
				code = IHLA_LEN_CSTRING;
				break;
			case 1:
				code = IHLA_LEN_1;
				break;
			case 2:
				code = IHLA_LEN_2;
				break;
			case 4:
				code = IHLA_LEN_4;
				break;
			case 8:
				code = IHLA_LEN_8;
				break;
			default:
				code = IHLA_LEN_OTHER;
				break;
		}
		*p++ = (char) (b | (code << IHLA_LEN_SHIFT));
		if (code == IHLA_LEN_OTHER)
		{
			uint16		len = (uint16) att->attlen;

			memcpy(p, &len, sizeof(uint16));
			p += sizeof(uint16);
		}
	}
	return p - buf;
}

/*
 * The old row's replica identity in heap's layout, as heap_update() and
 * heap_delete() log it (ExtractReplicaIdentity()): the whole row under
 * REPLICA IDENTITY FULL, otherwise its identity columns (the others null)
 * when key_required.  Values stored out of line are fetched, so that the
 * decoded row is complete; heap does that under the buffer lock as well.
 * Returns NULL if nothing is to be logged.
 */
static HeapTuple
old_identity(Relation rel, const Datum *values, const bool *isnull,
			 bool key_required, uint8 *lflags)
{
	TupleDesc	desc = RelationGetDescr(rel);
	char		replident = rel->rd_rel->relreplident;
	Bitmapset  *idattrs = NULL;
	Datum	   *v;
	bool	   *n;
	HeapTuple	tup;

	if (replident == REPLICA_IDENTITY_NOTHING)
		return NULL;
	if (replident != REPLICA_IDENTITY_FULL)
	{
		if (!key_required)
			return NULL;
		idattrs = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_IDENTITY_KEY);
		if (bms_is_empty(idattrs))
			return NULL;
	}

	v = palloc(Max(desc->natts, 1) * sizeof(Datum));
	n = palloc(Max(desc->natts, 1) * sizeof(bool));
	for (int i = 0; i < desc->natts; i++)
	{
		CompactAttribute *att = TupleDescCompactAttr(desc, i);

		v[i] = values[i];
		n[i] = isnull[i];
		if (idattrs != NULL &&
			!bms_is_member(i + 1 - FirstLowInvalidHeapAttributeNumber, idattrs))
			n[i] = true;
		if (!n[i] && att->attlen == -1 &&
			VARATT_IS_EXTERNAL(DatumGetPointer(v[i])))
			v[i] = PointerGetDatum(detoast_external_attr((varlena *)
														 DatumGetPointer(v[i])));
	}
	tup = heap_form_tuple(desc, v, n);
	*lflags |= (idattrs == NULL) ? XLIL_OLD_TUPLE : XLIL_OLD_KEY;
	bms_free(idattrs);
	return tup;
}

/*
 * Does an update need the old row's identity logged (REPLICA IDENTITY
 * DEFAULT or USING INDEX)?  As heap_update(): when an identity column
 * changed, or holds a value stored out of line (the new row refers to the
 * same TOAST value then, which the decoder did not see inserted).
 */
bool
iheap_logical_key_required(Relation rel, const Bitmapset *modified,
						   const Datum *oldvalues, const bool *oldisnull)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Bitmapset  *idattrs = RelationGetIndexAttrBitmap(rel,
													 INDEX_ATTR_BITMAP_IDENTITY_KEY);
	bool		result = bms_overlap(modified, idattrs);
	int			attidx = -1;

	while (!result && (attidx = bms_next_member(idattrs, attidx)) >= 0)
	{
		int			col = attidx + FirstLowInvalidHeapAttributeNumber - 1;

		if (col >= 0 && col < desc->natts && !oldisnull[col] &&
			TupleDescCompactAttr(desc, col)->attlen == -1 &&
			VARATT_IS_EXTERNAL(DatumGetPointer(oldvalues[col])))
			result = true;
	}
	bms_free(idattrs);
	return result;
}

/*
 * Prepare the logical part of a record (before its critical section: it
 * allocates).  layout: the record carries an iheap tuple of rel to decode.
 * oldvalues: the old row, whose replica identity is logged (key_required:
 * as for heap, whether the identity's columns changed, or a deletion).
 */
void
iheap_logical_prepare(IHeapLogicalPart *lp, Relation rel, bool layout,
					  const Datum *oldvalues, const bool *oldisnull,
					  bool key_required)
{
	TupleDesc	desc = RelationGetDescr(rel);
	HeapTuple	old = NULL;
	uint8		lflags = 0;
	Size		size;
	char	   *p;

	if (oldvalues != NULL)
		old = old_identity(rel, oldvalues, oldisnull, key_required, &lflags);
	if (layout)
		lflags |= XLIL_LAYOUT;

	size = 1;
	if (layout)
		size += sizeof(uint16) + desc->natts * 3;
	if (old != NULL)
		size += sizeof(uint32) + SizeOfHeapHeader +
			(old->t_len - SizeofHeapTupleHeader);

	lp->data = p = palloc(size);
	*p++ = (char) lflags;
	if (layout)
		p += layout_encode(desc, p);
	if (old != NULL)
	{
		uint32		len = old->t_len;
		xl_heap_header xh;

		xh.t_infomask2 = old->t_data->t_infomask2;
		xh.t_infomask = old->t_data->t_infomask;
		xh.t_hoff = old->t_data->t_hoff;
		memcpy(p, &len, sizeof(uint32));
		p += sizeof(uint32);
		memcpy(p, &xh, SizeOfHeapHeader);
		p += SizeOfHeapHeader;
		memcpy(p, (char *) old->t_data + SizeofHeapTupleHeader,
			   old->t_len - SizeofHeapTupleHeader);
		p += old->t_len - SizeofHeapTupleHeader;
		heap_freetuple(old);
	}
	lp->len = p - lp->data;
}

void
iheap_logical_free(IHeapLogicalPart *lp)
{
	if (lp->data)
		pfree(lp->data);
	lp->data = NULL;
	lp->len = 0;
}

/*
 * Do changes of rel need what logical decoding reads?  As heap: when
 * effective_wal_level is logical, for relations that are WAL-logged and
 * not catalogs, unless the caller says not to (TABLE_*_NO_LOGICAL).
 */
bool
iheap_logically_logged(Relation rel, bool no_logical)
{
	return !no_logical && RelationIsLogicallyLogged(rel);
}

/* ------------------------------------------------------------------------
 * Decoding
 * ------------------------------------------------------------------------
 */

/* the last layout decoded, with the descriptor built from it */
static MemoryContext layout_cxt = NULL;
static char *layout_bytes = NULL;
static Size layout_len = 0;
static TupleDesc layout_desc = NULL;

/* scratch memory of one record */
static MemoryContext decode_cxt = NULL;

/*
 * A descriptor with the attributes' length, alignment and by-value flag,
 * all that forming and deforming tuples (iheap's and heap's) looks at.
 * *len is set to the layout's length.
 */
static TupleDesc
layout_tupdesc(const char *layout, Size avail, Size *len)
{
	const char *p = layout;
	uint16		natts;
	TupleDesc	desc;
	MemoryContext oldcxt;

	if (avail < sizeof(uint16))
		elog(ERROR, "iheap: logical decoding: truncated layout");
	memcpy(&natts, p, sizeof(uint16));
	p += sizeof(uint16);

	/* the length, to compare with the last one */
	for (int i = 0; i < natts; i++)
	{
		if (p >= layout + avail)
			elog(ERROR, "iheap: logical decoding: truncated layout");
		if ((((uint8) *p) >> IHLA_LEN_SHIFT) == IHLA_LEN_OTHER)
			p += sizeof(uint16);
		p++;
	}
	if (p > layout + avail)
		elog(ERROR, "iheap: logical decoding: truncated layout");
	*len = p - layout;

	if (layout_desc != NULL && layout_len == *len &&
		memcmp(layout_bytes, layout, *len) == 0)
		return layout_desc;

	if (layout_cxt == NULL)
		layout_cxt = AllocSetContextCreate(TopMemoryContext,
										   "iheap logical decoding layout",
										   ALLOCSET_SMALL_SIZES);
	MemoryContextReset(layout_cxt);
	layout_desc = NULL;
	oldcxt = MemoryContextSwitchTo(layout_cxt);

	desc = CreateTemplateTupleDesc(natts);
	p = layout + sizeof(uint16);
	for (int i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		uint8		b = (uint8) *p++;

		memset(att, 0, ATTRIBUTE_FIXED_PART_SIZE);
		att->attnum = i + 1;
		att->atttypid = InvalidOid;
		att->atttypmod = -1;
		att->attalign = ihla_align_char[b & IHLA_ALIGN_MASK];
		att->attbyval = (b & IHLA_BYVAL) != 0;
		att->attstorage = (b & IHLA_PACKABLE) ? TYPSTORAGE_EXTENDED : TYPSTORAGE_PLAIN;
		switch (b >> IHLA_LEN_SHIFT)
		{
			case IHLA_LEN_VARLENA:
				att->attlen = -1;
				break;
			case IHLA_LEN_CSTRING:
				att->attlen = -2;
				break;
			case IHLA_LEN_1:
				att->attlen = 1;
				break;
			case IHLA_LEN_2:
				att->attlen = 2;
				break;
			case IHLA_LEN_4:
				att->attlen = 4;
				break;
			case IHLA_LEN_8:
				att->attlen = 8;
				break;
			case IHLA_LEN_OTHER:
				{
					uint16		l;

					memcpy(&l, p, sizeof(uint16));
					p += sizeof(uint16);
					att->attlen = (int16) l;
					break;
				}
			default:
				elog(ERROR, "iheap: logical decoding: bad attribute layout %02X", b);
		}
		if (att->attbyval && att->attlen != 1 && att->attlen != 2 &&
			att->attlen != 4 && att->attlen != 8)
			elog(ERROR, "iheap: logical decoding: bad attribute layout %02X", b);
		populate_compact_attribute(desc, i);
	}
	TupleDescFinalize(desc);

	layout_bytes = palloc(*len);
	memcpy(layout_bytes, layout, *len);
	layout_len = *len;
	layout_desc = desc;
	MemoryContextSwitchTo(oldcxt);
	return desc;
}

/* a heap tuple of the reorder buffer holding the heap tuple "src" */
static HeapTuple
reorder_tuple(ReorderBuffer *rb, HeapTuple src)
{
	HeapTuple	tup = ReorderBufferAllocTupleBuf(rb, src->t_len - SizeofHeapTupleHeader);

	tup->t_len = src->t_len;
	ItemPointerSetInvalid(&tup->t_self);
	/* known only once the transaction is replayed */
	tup->t_tableOid = InvalidOid;
	memcpy(tup->t_data, src->t_data, src->t_len);
	/* as DecodeXLogTuple(): no transaction fields */
	memset(tup->t_data, 0, offsetof(HeapTupleHeaderData, t_infomask2));
	return tup;
}

/*
 * The iheap tuple with header h and body (null bitmap and data, bodylen
 * bytes, unaligned, as in the record) as a heap tuple of the reorder buffer,
 * by the layout desc.  Records carry tuple headers in their WAL form
 * (§21 D42): their xid and undo pointer do not matter here.
 */
static HeapTuple
decode_iheap_tuple(ReorderBuffer *rb, TupleDesc desc, const IHeapHdr *h,
				   const char *body, uint32 bodylen)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(decode_cxt);
	uint32		hs = ih_hdr_size_of(h);
	uint32		tlen = hs + bodylen;
	char	   *mem = palloc(tlen + MAXIMUM_ALIGNOF);
	char	   *tup = mem;
	Datum	   *values;
	bool	   *isnull;
	HeapTuple	htup;
	HeapTuple	result;

	if (h->hoff < hs || h->hoff > tlen ||
		(h->infomask2 & IHT_NATTS_MASK) > desc->natts)
		elog(ERROR, "iheap: logical decoding: tuple does not match its layout");
	if (h->infomask & IHT_ALIGNDATA)
		tup = (char *) (TYPEALIGN(MAXIMUM_ALIGNOF, (uintptr_t) mem + h->hoff) - h->hoff);
	ih_hdr_write(tup, h);
	memcpy(tup + hs, body, bodylen);
	values = palloc(Max(desc->natts, 1) * sizeof(Datum));
	isnull = palloc(Max(desc->natts, 1) * sizeof(bool));
	ih_deform_tuple(desc, tup, values, isnull);
	htup = heap_form_tuple(desc, values, isnull);
	MemoryContextSwitchTo(oldcxt);

	result = reorder_tuple(rb, htup);
	MemoryContextReset(decode_cxt);
	return result;
}

/* the same for a tuple as a WAL header form followed by its body */
static HeapTuple
decode_wal_tuple(ReorderBuffer *rb, TupleDesc desc, const char *data,
				 uint32 len)
{
	IHeapHdr	h;

	if (len < SizeOfIHeapWalHdr)
		elog(ERROR, "iheap: logical decoding: truncated tuple");
	iheap_walhdr_get(data, &h, 0, FrozenTransactionId);
	return decode_iheap_tuple(rb, desc, &h, data + SizeOfIHeapWalHdr,
							  len - SizeOfIHeapWalHdr);
}

/* the old row's identity at "p" (heap's layout, as written) */
static HeapTuple
decode_old_tuple(ReorderBuffer *rb, const char *p, const char *end)
{
	uint32		len;
	xl_heap_header xh;
	HeapTuple	tup;

	if (p + sizeof(uint32) + SizeOfHeapHeader > end)
		elog(ERROR, "iheap: logical decoding: truncated old tuple");
	memcpy(&len, p, sizeof(uint32));
	p += sizeof(uint32);
	if (len < SizeofHeapTupleHeader ||
		p + SizeOfHeapHeader + (len - SizeofHeapTupleHeader) > end)
		elog(ERROR, "iheap: logical decoding: truncated old tuple");
	memcpy(&xh, p, SizeOfHeapHeader);
	p += SizeOfHeapHeader;

	tup = ReorderBufferAllocTupleBuf(rb, len - SizeofHeapTupleHeader);
	tup->t_len = len;
	ItemPointerSetInvalid(&tup->t_self);
	tup->t_tableOid = InvalidOid;
	memset(tup->t_data, 0, SizeofHeapTupleHeader);
	tup->t_data->t_infomask2 = xh.t_infomask2;
	tup->t_data->t_infomask = xh.t_infomask;
	tup->t_data->t_hoff = xh.t_hoff;
	memcpy((char *) tup->t_data + SizeofHeapTupleHeader, p,
		   len - SizeofHeapTupleHeader);
	return tup;
}

/* the logical part of a record whose struct takes "fixed" bytes */
typedef struct LogicalPart
{
	uint8		lflags;
	TupleDesc	desc;			/* XLIL_LAYOUT */
	const char *old;			/* XLIL_OLD_* */
	const char *end;
} LogicalPart;

static void
read_logical_part(XLogReaderState *r, Size fixed, LogicalPart *lp)
{
	const char *p = XLogRecGetData(r) + fixed;
	const char *end = XLogRecGetData(r) + XLogRecGetDataLen(r);

	if (XLogRecGetDataLen(r) < fixed + 1)
		elog(ERROR, "iheap: logical decoding: record without its logical part");
	lp->lflags = (uint8) *p++;
	lp->desc = NULL;
	lp->old = NULL;
	lp->end = end;
	if (lp->lflags & XLIL_LAYOUT)
	{
		Size		len;

		lp->desc = layout_tupdesc(p, end - p, &len);
		p += len;
	}
	if (lp->lflags & (XLIL_OLD_KEY | XLIL_OLD_TUPLE))
		lp->old = p;
}

static inline bool
filter_by_origin(LogicalDecodingContext *ctx, ReplOriginId origin_id)
{
	if (ctx->callbacks.filter_by_origin_cb == NULL)
		return false;
	return filter_by_origin_cb_wrapper(ctx, origin_id);
}

/* a change of the relation in block block_id, or NULL if not ours to queue */
static ReorderBufferChange *
new_change(LogicalDecodingContext *ctx, XLogReaderState *r, uint8 block_id,
		   ReorderBufferChangeType action)
{
	RelFileLocator rlocator;
	ReorderBufferChange *change;

	XLogRecGetBlockTag(r, block_id, &rlocator, NULL, NULL);
	if (rlocator.dbOid != ctx->slot->data.database)
		return NULL;
	if (filter_by_origin(ctx, XLogRecGetOrigin(r)))
		return NULL;

	change = ReorderBufferAllocChange(ctx->reorder);
	change->action = action;
	change->origin_id = XLogRecGetOrigin(r);
	change->data.tp.rlocator = rlocator;
	change->data.tp.clear_toast_afterwards = true;
	return change;
}

static void
queue_change(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
			 ReorderBufferChange *change)
{
	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(buf->record),
							 buf->origptr, change, false);
}

static void
decode_insert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	xl_iheap_insert *xlrec = (xl_iheap_insert *) XLogRecGetData(r);
	ReorderBufferChange *change;
	LogicalPart lp;
	char	   *tdata;
	Size		tlen;

	if (!(xlrec->flags & XLIH_LOGICAL))
		return;
	read_logical_part(r, SizeOfIHeapInsert, &lp);
	tdata = XLogRecGetBlockData(r, IH_BLK_DATA, &tlen);
	if (lp.desc == NULL || tdata == NULL)
		elog(ERROR, "iheap: logical decoding: insert without its tuple");

	if (tlen < SizeOfIHeapWalHdr)
		elog(ERROR, "iheap: logical decoding: insert without its tuple");
	change = new_change(ctx, r, IH_BLK_DATA,
						(ih_hdr_infomask(tdata) & IHT_SPECULATIVE) ?
						REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT :
						REORDER_BUFFER_CHANGE_INSERT);
	if (change == NULL)
		return;
	change->data.tp.newtuple = decode_wal_tuple(ctx->reorder, lp.desc,
												tdata, tlen);
	queue_change(ctx, buf, change);
}

static void
decode_multi_insert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	xl_iheap_multi_insert *xlrec = (xl_iheap_multi_insert *) XLogRecGetData(r);
	LogicalPart lp;
	char	   *data;
	char	   *end;
	Size		len;

	if (!(xlrec->flags & XLIH_LOGICAL))
		return;
	read_logical_part(r, SizeOfIHeapMultiInsert, &lp);
	data = XLogRecGetBlockData(r, IH_BLK_DATA, &len);
	if (lp.desc == NULL || data == NULL)
		elog(ERROR, "iheap: logical decoding: multi-insert without its tuples");
	end = data + len;

	for (int i = 0; i < xlrec->ntuples; i++)
	{
		ReorderBufferChange *change;
		uint16		bodylen;
		IHeapHdr	h;

		/* offnum, body length, infomask2, infomask, hoff, body */
		if (data + SizeOfIHeapMultiTuple > end)
			elog(ERROR, "iheap: logical decoding: truncated multi-insert");
		memcpy(&bodylen, data + 2, sizeof(uint16));
		memcpy(&h.infomask2, data + 4, sizeof(uint16));
		memcpy(&h.infomask, data + 6, sizeof(uint16));
		h.hoff = (uint8) data[8];
		h.urec = InvalidUndoRecPtr;
		h.xid = FrozenTransactionId;
		data += SizeOfIHeapMultiTuple;
		if (data + bodylen > end)
			elog(ERROR, "iheap: logical decoding: truncated multi-insert");

		change = new_change(ctx, r, IH_BLK_DATA, REORDER_BUFFER_CHANGE_INSERT);
		if (change == NULL)
			return;
		change->data.tp.newtuple = decode_iheap_tuple(ctx->reorder, lp.desc,
													  &h, data, bodylen);

		/*
		 * As heap: TOAST reassembly state is reset only after the last row
		 * of one multi_insert call, whose values were all toasted first.
		 */
		change->data.tp.clear_toast_afterwards =
			(xlrec->flags & XLIH_LAST_IN_MULTI) && i + 1 == xlrec->ntuples;
		queue_change(ctx, buf, change);
		data += bodylen;
	}
}

/* UPDATE (in place) and MOVE: an UPDATE change */
static void
decode_update(LogicalDecodingContext *ctx, XLogRecordBuffer *buf, bool move)
{
	XLogReaderState *r = buf->record;
	ReorderBufferChange *change;
	LogicalPart lp;
	char	   *tdata;
	Size		tlen;
	uint8		block_id;

	if (move)
	{
		xl_iheap_move *xlrec = (xl_iheap_move *) XLogRecGetData(r);

		if (!(xlrec->flags & XLIH_LOGICAL))
			return;
		read_logical_part(r, SizeOfIHeapMove, &lp);
		if (xlrec->flags & XLIH_SAME_PAGE)
		{
			/* the old item's header change, then the new tuple */
			tdata = XLogRecGetBlockData(r, IH_BLK_DATA, &tlen);
			if (tdata == NULL || tlen < SizeOfIHeapWalHdrChange)
				elog(ERROR, "iheap: logical decoding: update without its tuple");
			tdata += SizeOfIHeapWalHdrChange;
			tlen -= SizeOfIHeapWalHdrChange;
		}
		else
			tdata = XLogRecGetBlockData(r, IH_BLK_NEWDATA, &tlen);
		block_id = IH_BLK_DATA;
	}
	else
	{
		xl_iheap_update *xlrec = (xl_iheap_update *) XLogRecGetData(r);

		if (!(xlrec->flags & XLIH_LOGICAL))
			return;
		read_logical_part(r, SizeOfIHeapUpdate, &lp);
		/* logged whole: the header's WAL form, then the body (no prefix, no suffix) */
		tdata = XLogRecGetBlockData(r, IH_BLK_DATA, &tlen);
		if (xlrec->prefix != 0 || xlrec->suffix != 0)
			elog(ERROR, "iheap: logical decoding: update logged as a delta");
		block_id = IH_BLK_DATA;
	}
	if (lp.desc == NULL || tdata == NULL)
		elog(ERROR, "iheap: logical decoding: update without its tuple");

	change = new_change(ctx, r, block_id, REORDER_BUFFER_CHANGE_UPDATE);
	if (change == NULL)
		return;
	change->data.tp.newtuple = decode_wal_tuple(ctx->reorder, lp.desc,
												tdata, tlen);
	if (lp.old != NULL)
		change->data.tp.oldtuple = decode_old_tuple(ctx->reorder, lp.old,
													lp.end);
	queue_change(ctx, buf, change);
}

static void
decode_delete(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	xl_iheap_hdrchange *xlrec = (xl_iheap_hdrchange *) XLogRecGetData(r);
	ReorderBufferChange *change;
	LogicalPart lp;

	if (!(xlrec->flags & XLIH_LOGICAL))
		return;
	read_logical_part(r, SizeOfIHeapHdrChange, &lp);

	change = new_change(ctx, r, IH_BLK_DATA, REORDER_BUFFER_CHANGE_DELETE);
	if (change == NULL)
		return;
	if (lp.old != NULL)
		change->data.tp.oldtuple = decode_old_tuple(ctx->reorder, lp.old,
													lp.end);
	queue_change(ctx, buf, change);
}

/* the end of a speculative insertion: its confirmation, or super-deletion */
static void
decode_spec_finish(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	xl_iheap_spec_finish *xlrec = (xl_iheap_spec_finish *) XLogRecGetData(r);
	ReorderBufferChange *change;

	if (!(xlrec->flags & XLIH_SPEC_LOGICAL))
		return;
	change = new_change(ctx, r, IH_BLK_DATA,
						(xlrec->flags & XLIH_SPEC_SUCCEEDED) ?
						REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM :
						REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT);
	if (change == NULL)
		return;
	queue_change(ctx, buf, change);
}

/*
 * rm_decode.  As heap_decode(): every record's xid is made known to the
 * reorder buffer; data changes are decoded once there is a snapshot to
 * decode them with, except while fast-forwarding (where the base snapshot
 * is still built) or for a backend decoding on behalf of REPACK
 * (CONCURRENTLY), which repacks heap tables only.  Changes of other
 * databases and filtered origins are passed over.  iheap.logical_decoding
 * = ignore passes over every change of iheap tables.
 */
void
iheap_decode(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	uint8		info = XLogRecGetInfo(r) & XLOG_IHEAP_OPMASK;
	TransactionId xid = XLogRecGetXid(r);
	SnapBuild  *builder = ctx->snapshot_builder;

	ReorderBufferProcessXid(ctx->reorder, xid, buf->origptr);

	if (SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT)
		return;

	switch (info)
	{
		case XLOG_IHEAP_INSERT:
		case XLOG_IHEAP_MULTI_INSERT:
		case XLOG_IHEAP_UPDATE:
		case XLOG_IHEAP_MOVE:
		case XLOG_IHEAP_DELETE:
		case XLOG_IHEAP_SPEC_FINISH:
			break;
		default:
			/* physical: locks, rollback, pruning, undo, the metapage */
			return;
	}

	if (!SnapBuildProcessChange(builder, xid, buf->origptr) ||
		ctx->fast_forward || change_useless_for_repack(buf) ||
		iheap_logical_decoding == IHEAP_LOGICAL_IGNORE)
		return;

	if (decode_cxt == NULL)
		decode_cxt = AllocSetContextCreate(TopMemoryContext,
										   "iheap logical decoding",
										   ALLOCSET_DEFAULT_SIZES);

	switch (info)
	{
		case XLOG_IHEAP_INSERT:
			decode_insert(ctx, buf);
			break;
		case XLOG_IHEAP_MULTI_INSERT:
			decode_multi_insert(ctx, buf);
			break;
		case XLOG_IHEAP_UPDATE:
			decode_update(ctx, buf, false);
			break;
		case XLOG_IHEAP_MOVE:
			decode_update(ctx, buf, true);
			break;
		case XLOG_IHEAP_DELETE:
			decode_delete(ctx, buf);
			break;
		case XLOG_IHEAP_SPEC_FINISH:
			decode_spec_finish(ctx, buf);
			break;
	}
}
