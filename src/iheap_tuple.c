/*-------------------------------------------------------------------------
 *
 * iheap_tuple.c
 *	  Forming and deforming iheap tuples.
 *
 * Layout (DESIGN.md §21 D2, D11): a 16-byte header, an optional null
 * bitmap, then the data area.  Compared to heap tuples:
 *
 *	- pass-by-value attributes are stored without alignment padding and are
 *	  read with memcpy;
 *	- pass-by-reference attributes follow heap's rules (short varlenas are
 *	  unaligned, everything else is aligned to attalignby) but relative to
 *	  the start of the data area.  A tuple holding at least one value that
 *	  needs alignment is flagged IHT_ALIGNDATA, and whoever places it in
 *	  memory (a page, a slot) makes the data area MAXALIGNed.
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupdesc.h"
#include "access/tupmacs.h"
#include "executor/tuptable.h"
#include "utils/expandeddatum.h"
#include "utils/memutils.h"
#include "varatt.h"

#include "iheap.h"

static inline Datum
ih_fetch_byval(const char *p, int len)
{
	switch (len)
	{
		case sizeof(char):
			return CharGetDatum(*p);
		case sizeof(int16):
			{
				int16		v;

				memcpy(&v, p, sizeof(v));
				return Int16GetDatum(v);
			}
		case sizeof(int32):
			{
				int32		v;

				memcpy(&v, p, sizeof(v));
				return Int32GetDatum(v);
			}
		default:
			{
				int64		v;

				Assert(len == sizeof(int64));
				memcpy(&v, p, sizeof(v));
				return Int64GetDatum(v);
			}
	}
}

static inline void
ih_store_byval(char *p, Datum d, int len)
{
	switch (len)
	{
		case sizeof(char):
			*p = DatumGetChar(d);
			break;
		case sizeof(int16):
			{
				int16		v = DatumGetInt16(d);

				memcpy(p, &v, sizeof(v));
				break;
			}
		case sizeof(int32):
			{
				int32		v = DatumGetInt32(d);

				memcpy(p, &v, sizeof(v));
				break;
			}
		case sizeof(int64):
			{
				int64		v = DatumGetInt64(d);

				memcpy(p, &v, sizeof(v));
				break;
			}
		default:
			elog(ERROR, "unsupported byval length: %d", len);
	}
}

/*
 * Size of the data area for the given values.  *needs_align is set when
 * some value has to be aligned to more than one byte.
 */
Size
ih_compute_data_size(TupleDesc desc, const Datum *values, const bool *isnull,
					 bool *needs_align)
{
	Size		len = 0;

	*needs_align = false;

	for (int i = 0; i < desc->natts; i++)
	{
		CompactAttribute *att;
		Datum		val;

		if (isnull[i])
			continue;

		att = TupleDescCompactAttr(desc, i);
		val = values[i];

		if (att->attbyval)
		{
			len += att->attlen;
		}
		else if (att->attlen == -1)
		{
			Pointer		p = DatumGetPointer(val);

			if (VARATT_IS_EXTERNAL(p))
			{
				if (VARATT_IS_EXTERNAL_EXPANDED(p))
				{
					len = TYPEALIGN(att->attalignby, len);
					if (att->attalignby > 1)
						*needs_align = true;
					len += EOH_get_flat_size(DatumGetEOHP(val));
				}
				else
					len += VARSIZE_EXTERNAL(p);
			}
			else if (VARATT_IS_SHORT(p))
				len += VARSIZE_SHORT(p);
			else if (att->attispackable && VARATT_CAN_MAKE_SHORT(p))
				len += VARATT_CONVERTED_SHORT_SIZE(p);
			else
			{
				len = TYPEALIGN(att->attalignby, len);
				if (att->attalignby > 1)
					*needs_align = true;
				len += VARSIZE(p);
			}
		}
		else if (att->attlen == -2)
		{
			len += strlen(DatumGetCString(val)) + 1;
		}
		else
		{
			len = TYPEALIGN(att->attalignby, len);
			if (att->attalignby > 1)
				*needs_align = true;
			len += att->attlen;
		}
	}

	return len;
}

/*
 * Fill the data area.  "data" must be zeroed and "len" bytes long.
 */
static void
ih_fill_data(TupleDesc desc, const Datum *values, const bool *isnull,
			 char *data, Size len, uint16 *infomask, uint8 *bits)
{
	Size		off = 0;

	for (int i = 0; i < desc->natts; i++)
	{
		CompactAttribute *att = TupleDescCompactAttr(desc, i);
		Datum		val = values[i];
		Size		datalen;

		if (bits != NULL)
		{
			if (isnull[i])
			{
				*infomask |= IHT_HASNULL;
				continue;
			}
			bits[i >> 3] |= (1 << (i & 7));
		}
		else
			Assert(!isnull[i]);

		if (att->attbyval)
		{
			ih_store_byval(data + off, val, att->attlen);
			datalen = att->attlen;
		}
		else if (att->attlen == -1)
		{
			Pointer		p = DatumGetPointer(val);

			*infomask |= IHT_HASVARWIDTH;
			if (VARATT_IS_EXTERNAL(p))
			{
				if (VARATT_IS_EXTERNAL_EXPANDED(p))
				{
					ExpandedObjectHeader *eoh = DatumGetEOHP(val);

					off = TYPEALIGN(att->attalignby, off);
					datalen = EOH_get_flat_size(eoh);
					EOH_flatten_into(eoh, data + off, datalen);
				}
				else
				{
					*infomask |= IHT_HASEXTERNAL;
					datalen = VARSIZE_EXTERNAL(p);
					memcpy(data + off, p, datalen);
				}
			}
			else if (VARATT_IS_SHORT(p))
			{
				datalen = VARSIZE_SHORT(p);
				memcpy(data + off, p, datalen);
			}
			else if (att->attispackable && VARATT_CAN_MAKE_SHORT(p))
			{
				datalen = VARATT_CONVERTED_SHORT_SIZE(p);
				SET_VARSIZE_SHORT(data + off, datalen);
				memcpy(data + off + 1, VARDATA(p), datalen - 1);
			}
			else
			{
				off = TYPEALIGN(att->attalignby, off);
				datalen = VARSIZE(p);
				memcpy(data + off, p, datalen);
			}
		}
		else if (att->attlen == -2)
		{
			*infomask |= IHT_HASVARWIDTH;
			datalen = strlen(DatumGetCString(val)) + 1;
			memcpy(data + off, DatumGetPointer(val), datalen);
		}
		else
		{
			off = TYPEALIGN(att->attalignby, off);
			datalen = att->attlen;
			memcpy(data + off, DatumGetPointer(val), datalen);
		}

		off += datalen;
	}

	if (off != len)
		elog(ERROR, "iheap tuple data size mismatch: computed %zu, filled %zu",
			 len, off);
}

/*
 * Position inside "mem" (of at least len + MAXIMUM_ALIGNOF bytes) where a
 * tuple with the given header length must start so that its data area is
 * MAXALIGNed.
 */
static inline char *
ih_aligned_start(char *mem, uint8 hoff, bool needs_align)
{
	uintptr_t	p;

	if (!needs_align)
		return mem;
	p = (uintptr_t) mem + hoff;
	p = TYPEALIGN(MAXIMUM_ALIGNOF, p);
	return (char *) (p - hoff);
}

/*
 * Form a tuple in palloc'd memory of the current context.  Returns the
 * tuple, positioned so that its data area is aligned if needed; *mem is the
 * allocation to pfree.  Transaction fields are left zero.
 */
char *
ih_form_tuple(TupleDesc desc, const Datum *values, const bool *isnull,
			  uint32 *len, char **mem)
{
	bool		hasnull = false;
	bool		needs_align;
	Size		datalen;
	Size		hoff;
	Size		total;
	char	   *tup;
	IHeapHdr	h;
	uint16		infomask = 0;

	if (desc->natts > IHT_NATTS_MASK)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("number of columns (%d) exceeds limit (%d)",
						desc->natts, IHT_NATTS_MASK)));

	for (int i = 0; i < desc->natts; i++)
	{
		if (isnull[i])
		{
			hasnull = true;
			break;
		}
	}

	hoff = IHEAP_TUPHDR_SIZE + (hasnull ? BITMAPLEN(desc->natts) : 0);
	datalen = ih_compute_data_size(desc, values, isnull, &needs_align);
	/*
	 * In-memory tuples (slots) may be of any size; the size limit for a
	 * tuple on a page is enforced where it is placed.
	 */
	total = hoff + datalen;
	if (total > MaxAllocSize - MAXIMUM_ALIGNOF)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("row is too big: size %zu", total)));

	*mem = palloc0(total + MAXIMUM_ALIGNOF);
	tup = ih_aligned_start(*mem, (uint8) hoff, needs_align);

	ih_fill_data(desc, values, isnull, tup + hoff, datalen, &infomask,
				 hasnull ? (uint8 *) (tup + IHEAP_TUPHDR_SIZE) : NULL);
	if (needs_align)
		infomask |= IHT_ALIGNDATA;

	memset(&h, 0, sizeof(h));
	h.infomask2 = (uint16) desc->natts;
	h.infomask = infomask;
	h.hoff = (uint8) hoff;
	ih_hdr_write(tup, &h);

	*len = (uint32) total;
	return tup;
}

/*
 * Deform the first "natts" attributes of "tuple" into the slot, continuing
 * from slot->tts_nvalid / *offp (incremental, like heap's slot deforming).
 */
void
ih_deform_into_slot(TupleTableSlot *slot, const char *tuple, uint32 *offp,
					int natts)
{
	TupleDesc	desc = slot->tts_tupleDescriptor;
	Datum	   *values = slot->tts_values;
	bool	   *isnull = slot->tts_isnull;
	int			tupnatts = ih_hdr_natts(tuple);
	uint16		infomask = ih_hdr_infomask(tuple);
	bool		hasnull = (infomask & IHT_HASNULL) != 0;
	const uint8 *bits = (const uint8 *) (tuple + IHEAP_TUPHDR_SIZE);
	const char *tp = tuple + ih_hdr_hoff(tuple);
	int			attnum = slot->tts_nvalid;
	uint32		off = *offp;
	int			maxatt = Min(natts, tupnatts);

	for (; attnum < maxatt; attnum++)
	{
		CompactAttribute *att;

		if (hasnull && att_isnull(attnum, bits))
		{
			values[attnum] = (Datum) 0;
			isnull[attnum] = true;
			continue;
		}

		isnull[attnum] = false;
		att = TupleDescCompactAttr(desc, attnum);

		if (att->attbyval)
		{
			values[attnum] = ih_fetch_byval(tp + off, att->attlen);
			off += att->attlen;
		}
		else if (att->attlen == -1)
		{
			if (!VARATT_IS_SHORT(tp + off))
				off = TYPEALIGN(att->attalignby, off);
			values[attnum] = PointerGetDatum(tp + off);
			off += VARSIZE_ANY(tp + off);
		}
		else if (att->attlen == -2)
		{
			values[attnum] = PointerGetDatum(tp + off);
			off += strlen(tp + off) + 1;
		}
		else
		{
			off = TYPEALIGN(att->attalignby, off);
			values[attnum] = PointerGetDatum(tp + off);
			off += att->attlen;
		}
	}

	*offp = off;
	if (attnum < natts)
	{
		slot->tts_nvalid = attnum;
		slot_getmissingattrs(slot, attnum, natts);
	}
	slot->tts_nvalid = natts;
}

/*
 * Deform all attributes of "tuple" into values/isnull (desc->natts entries),
 * filling in missing attributes.
 */
void
ih_deform_tuple(TupleDesc desc, const char *tuple, Datum *values, bool *isnull)
{
	int			tupnatts = ih_hdr_natts(tuple);
	uint16		infomask = ih_hdr_infomask(tuple);
	bool		hasnull = (infomask & IHT_HASNULL) != 0;
	const uint8 *bits = (const uint8 *) (tuple + IHEAP_TUPHDR_SIZE);
	const char *tp = tuple + ih_hdr_hoff(tuple);
	uint32		off = 0;
	int			natts = desc->natts;
	int			attnum;

	for (attnum = 0; attnum < Min(natts, tupnatts); attnum++)
	{
		CompactAttribute *att;

		if (hasnull && att_isnull(attnum, bits))
		{
			values[attnum] = (Datum) 0;
			isnull[attnum] = true;
			continue;
		}

		isnull[attnum] = false;
		att = TupleDescCompactAttr(desc, attnum);

		if (att->attbyval)
		{
			values[attnum] = ih_fetch_byval(tp + off, att->attlen);
			off += att->attlen;
		}
		else if (att->attlen == -1)
		{
			if (!VARATT_IS_SHORT(tp + off))
				off = TYPEALIGN(att->attalignby, off);
			values[attnum] = PointerGetDatum(tp + off);
			off += VARSIZE_ANY(tp + off);
		}
		else if (att->attlen == -2)
		{
			values[attnum] = PointerGetDatum(tp + off);
			off += strlen(tp + off) + 1;
		}
		else
		{
			off = TYPEALIGN(att->attalignby, off);
			values[attnum] = PointerGetDatum(tp + off);
			off += att->attlen;
		}
	}

	for (; attnum < natts; attnum++)
		values[attnum] = getmissingattr(desc, attnum + 1, &isnull[attnum]);
}

/*
 * Copy a tuple into memory of "cxt", positioned so that its data area is
 * aligned when the tuple needs it.  *mem receives the allocation.
 */
char *
ih_copy_tuple_aligned(MemoryContext cxt, const char *tuple, uint32 len,
					  char **mem)
{
	char	   *m = MemoryContextAlloc(cxt, len + MAXIMUM_ALIGNOF);
	char	   *t = ih_aligned_start(m, ih_hdr_hoff(tuple),
									 ih_tuple_needs_align(tuple));

	memcpy(t, tuple, len);
	*mem = m;
	return t;
}

uint32
ih_tuple_len_on_page(Page page, OffsetNumber off)
{
	ItemId		lp = PageGetItemId(page, off);

	return ItemIdGetLength(lp);
}
