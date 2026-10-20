/*-------------------------------------------------------------------------
 *
 * iheap_undo.c
 *	  Undo storage: page allocation, record writing and fetching.
 *
 * Each backend appends to its own undo page per relation (DESIGN.md §21
 * D4).  A new logical page is taken from the metapage under its exclusive
 * lock; the WAL record that does so also initializes the page.  When the
 * logical page lies beyond the live extents, a new extent is allocated:
 * a free extent is reused, otherwise the relation is extended by a
 * contiguous run of pages while holding the relation extension lock.
 *
 * Lock order: data page(s) -> undo page -> metapage -> extension lock.
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "iheap.h"
#include "iheap_xlog.h"

typedef struct UndoAppendEnt
{
	RelFileLocator key;
	uint64		lpage;			/* 0: none */
	BlockNumber blkno;
	uint32		nalloc;			/* pages allocated since last discard try */
} UndoAppendEnt;

static HTAB *undo_append_htab = NULL;

static UndoAppendEnt *
undo_append_entry(Relation rel)
{
	UndoAppendEnt *ent;
	bool		found;

	if (undo_append_htab == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(RelFileLocator);
		ctl.entrysize = sizeof(UndoAppendEnt);
		ctl.hcxt = TopMemoryContext;
		undo_append_htab = hash_create("iheap undo append points", 64, &ctl,
									   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	ent = hash_search(undo_append_htab, &rel->rd_locator, HASH_ENTER, &found);
	if (!found)
	{
		ent->lpage = 0;
		ent->blkno = InvalidBlockNumber;
		ent->nalloc = 0;
	}
	return ent;
}

void
iheap_undo_forget_relation(RelFileLocator rlocator)
{
	if (undo_append_htab)
		hash_search(undo_append_htab, &rlocator, HASH_REMOVE, NULL);
}

/* called when shared state may have been lost (e.g. after recovery) */
void
iheap_undo_reset_backend(void)
{
	if (undo_append_htab)
	{
		hash_destroy(undo_append_htab);
		undo_append_htab = NULL;
	}
}

/* pages allocated by this backend since the last discard attempt */
uint32
iheap_undo_take_alloc_count(Relation rel)
{
	UndoAppendEnt *ent;
	uint32		n;

	if (undo_append_htab == NULL)
		return 0;
	ent = hash_search(undo_append_htab, &rel->rd_locator, HASH_FIND, NULL);
	if (ent == NULL)
		return 0;
	n = ent->nalloc;
	ent->nalloc = 0;
	return n;
}

void
iheap_undo_init_page(Page page, uint64 lpage)
{
	IHeapUndoPageHeaderData *uph;

	PageInit(page, BLCKSZ, IHEAP_SPECIAL_SIZE);
	IHeapPageGetOpaque(page)->ihp_type = IHEAP_PAGE_UNDO;
	uph = IHeapUndoPageGetHeader(page);
	uph->uph_lpage = lpage;
	uph->uph_min_xid = InvalidTransactionId;
	uph->uph_max_xid = InvalidTransactionId;
	uph->uph_nrecs = 0;
	uph->uph_format = IHEAP_UNDO_FORMAT_COMPACT;
	((PageHeader) page)->pd_lower = IHEAP_UNDO_DATA_START;
}

/* ------------------------------------------------------------------------
 * Compact record headers (§21 D37)
 *
 * A record on a page of format IHEAP_UNDO_FORMAT_COMPACT starts with
 *
 *		ur_type | ur_flags << 4 (1), format bits IHUC_* (1),
 *		payload length (1 or 2), ur_xid (4), ur_cid (2 or 4),
 *		ur_blkno (2 or 4), ur_offnum (1 or 2),
 *		ur_xact_prev (0, 2: an offset on this page, or 7),
 *		ur_prev_hdr: nothing for an insert (all zero); else infomask2,
 *			infomask, hoff (5) and xid (4), then its undo pointer:
 *			nothing (0: the version was frozen), 2 (an offset on this
 *			page) or 7
 *
 * then the payload.  Every field is little endian and unaligned.  12 to
 * IHEAP_UNDOREC_MAXHDRSZ bytes, where format IHEAP_UNDO_FORMAT_V1 took 42:
 * an insert's header takes 12 to 16, an in-place update's 21 (of a frozen
 * row) to 30.  What a record encodes depends only on the header and the
 * page's logical number, so redo, which appends what WAL carries in a form
 * of its own (iheap_undo_register_wal), builds the same bytes.
 * ------------------------------------------------------------------------
 */
#define IHUC_XP_MASK		0x03	/* ur_xact_prev: */
#define IHUC_XP_NONE		0x00	/* invalid */
#define IHUC_XP_LOCAL		0x01	/* an offset on this page */
#define IHUC_XP_FULL		0x02	/* 7 bytes */
#define IHUC_PH_MASK		0x0C	/* ur_prev_hdr: */
#define IHUC_PH_NONE		0x00	/* all zero (inserts) */
#define IHUC_PH_FROZEN		0x04	/* undo pointer 0 */
#define IHUC_PH_LOCAL		0x08	/* undo pointer on this page */
#define IHUC_PH_FULL		0x0C	/* undo pointer, 7 bytes */
#define IHUC_LEN8			0x10	/* payload length in 1 byte */
#define IHUC_CID16			0x20	/* ur_cid in 2 bytes */
#define IHUC_BLK16			0x40	/* ur_blkno in 2 bytes */
#define IHUC_OFF8			0x80	/* ur_offnum in 1 byte */

/* the first byte holds the type and the flags, four bits each */
StaticAssertDecl(IHU_LOCK <= 0x0F &&
				 ((IHUF_SPEC | IHUF_LOCKERS | IHUF_KEYS_UPDATED | IHUF_MOVED_IN) & ~0x0F) == 0,
				 "undo record types and flags must fit in four bits each");

static inline char *
put_uint(char *p, uint64 v, int nbytes)
{
	for (int i = 0; i < nbytes; i++)
	{
		p[i] = (char) (v & 0xFF);
		v >>= 8;
	}
	return p + nbytes;
}

static inline const char *
get_uint(const char *p, uint64 *v, int nbytes)
{
	uint64		r = 0;

	for (int i = nbytes - 1; i >= 0; i--)
		r = (r << 8) | (uint8) p[i];
	*v = r;
	return p + nbytes;
}

/* the compact header of hdr, for a record on logical undo page lpage */
static uint32
undo_encode(char *dst, const IHeapUndoRecHeader *hdr, uint32 payload_len,
			uint64 lpage)
{
	static const char zero[IHEAP_TUPHDR_SIZE] = {0};
	char	   *p = dst + 2;
	uint8		fmt = 0;

	if (payload_len <= PG_UINT8_MAX)
	{
		fmt |= IHUC_LEN8;
		p = put_uint(p, payload_len, 1);
	}
	else
		p = put_uint(p, payload_len, 2);
	p = put_uint(p, hdr->ur_xid, 4);
	if (hdr->ur_cid <= PG_UINT16_MAX)
	{
		fmt |= IHUC_CID16;
		p = put_uint(p, hdr->ur_cid, 2);
	}
	else
		p = put_uint(p, hdr->ur_cid, 4);
	if (hdr->ur_blkno <= PG_UINT16_MAX)
	{
		fmt |= IHUC_BLK16;
		p = put_uint(p, hdr->ur_blkno, 2);
	}
	else
		p = put_uint(p, hdr->ur_blkno, 4);
	if (hdr->ur_offnum <= PG_UINT8_MAX)
	{
		fmt |= IHUC_OFF8;
		p = put_uint(p, hdr->ur_offnum, 1);
	}
	else
		p = put_uint(p, hdr->ur_offnum, 2);

	if (!UndoRecPtrIsValid(hdr->ur_xact_prev))
		fmt |= IHUC_XP_NONE;
	else if (UndoRecPtrGetLPage(hdr->ur_xact_prev) == lpage)
	{
		fmt |= IHUC_XP_LOCAL;
		p = put_uint(p, UndoRecPtrGetOffset(hdr->ur_xact_prev), 2);
	}
	else
	{
		fmt |= IHUC_XP_FULL;
		p = put_uint(p, hdr->ur_xact_prev, 7);
	}

	if (memcmp(hdr->ur_prev_hdr, zero, IHEAP_TUPHDR_SIZE) == 0)
		fmt |= IHUC_PH_NONE;
	else
	{
		IHeapHdr	ph;

		ih_hdr_read(hdr->ur_prev_hdr, &ph);
		p = put_uint(p, ph.infomask2, 2);
		p = put_uint(p, ph.infomask, 2);
		p = put_uint(p, ph.hoff, 1);
		p = put_uint(p, ph.xid, 4);
		if (!UndoRecPtrIsValid(ph.urec))
			fmt |= IHUC_PH_FROZEN;
		else if (UndoRecPtrGetLPage(ph.urec) == lpage)
		{
			fmt |= IHUC_PH_LOCAL;
			p = put_uint(p, UndoRecPtrGetOffset(ph.urec), 2);
		}
		else
		{
			fmt |= IHUC_PH_FULL;
			p = put_uint(p, ph.urec, 7);
		}
	}

	dst[0] = (char) ((hdr->ur_type & 0x0F) | (hdr->ur_flags << 4));
	dst[1] = (char) fmt;
	return p - dst;
}

/*
 * Decode the compact header at src, which has "avail" bytes before the end
 * of the page's records.  Returns false if they do not hold the header and
 * its payload.
 */
static bool
undo_decode(const char *src, uint32 avail, uint64 lpage,
			IHeapUndoRecHeader *hdr, uint32 *hdrlen, uint32 *payload_len)
{
	const char *p = src + 2;
	uint8		fmt;
	uint64		v;
	uint32		need;

	if (avail < 2)
		return false;
	fmt = (uint8) src[1];

	/* the header's length follows from the format bits */
	need = 2 + ((fmt & IHUC_LEN8) ? 1 : 2) + 4 + ((fmt & IHUC_CID16) ? 2 : 4) +
		((fmt & IHUC_BLK16) ? 2 : 4) + ((fmt & IHUC_OFF8) ? 1 : 2);
	switch (fmt & IHUC_XP_MASK)
	{
		case IHUC_XP_NONE:
			break;
		case IHUC_XP_LOCAL:
			need += 2;
			break;
		case IHUC_XP_FULL:
			need += 7;
			break;
		default:
			return false;
	}
	switch (fmt & IHUC_PH_MASK)
	{
		case IHUC_PH_NONE:
			break;
		case IHUC_PH_FROZEN:
			need += 9;
			break;
		case IHUC_PH_LOCAL:
			need += 11;
			break;
		default:
			need += 16;
			break;
	}
	if (avail < need)
		return false;

	memset(hdr, 0, sizeof(*hdr));
	hdr->ur_type = (uint8) src[0] & 0x0F;
	hdr->ur_flags = (uint8) src[0] >> 4;
	p = get_uint(p, &v, (fmt & IHUC_LEN8) ? 1 : 2);
	*payload_len = (uint32) v;
	p = get_uint(p, &v, 4);
	hdr->ur_xid = (TransactionId) v;
	p = get_uint(p, &v, (fmt & IHUC_CID16) ? 2 : 4);
	hdr->ur_cid = (CommandId) v;
	p = get_uint(p, &v, (fmt & IHUC_BLK16) ? 2 : 4);
	hdr->ur_blkno = (BlockNumber) v;
	p = get_uint(p, &v, (fmt & IHUC_OFF8) ? 1 : 2);
	hdr->ur_offnum = (OffsetNumber) v;

	switch (fmt & IHUC_XP_MASK)
	{
		case IHUC_XP_LOCAL:
			p = get_uint(p, &v, 2);
			hdr->ur_xact_prev = MakeUndoRecPtr(lpage, v);
			break;
		case IHUC_XP_FULL:
			p = get_uint(p, &v, 7);
			hdr->ur_xact_prev = v;
			break;
		default:
			hdr->ur_xact_prev = InvalidUndoRecPtr;
			break;
	}

	if ((fmt & IHUC_PH_MASK) != IHUC_PH_NONE)
	{
		IHeapHdr	ph;

		p = get_uint(p, &v, 2);
		ph.infomask2 = (uint16) v;
		p = get_uint(p, &v, 2);
		ph.infomask = (uint16) v;
		p = get_uint(p, &v, 1);
		ph.hoff = (uint8) v;
		p = get_uint(p, &v, 4);
		ph.xid = (TransactionId) v;
		switch (fmt & IHUC_PH_MASK)
		{
			case IHUC_PH_LOCAL:
				p = get_uint(p, &v, 2);
				ph.urec = MakeUndoRecPtr(lpage, v);
				break;
			case IHUC_PH_FULL:
				p = get_uint(p, &v, 7);
				ph.urec = v;
				break;
			default:
				ph.urec = InvalidUndoRecPtr;
				break;
		}
		ih_hdr_write(hdr->ur_prev_hdr, &ph);
	}

	Assert(p - src == need);
	if (avail - need < *payload_len)
		return false;
	*hdrlen = need;
	hdr->ur_len = (uint16) (need + *payload_len);
	return true;
}

/*
 * Read the record at offset "off" of an undo page whose records end at
 * "limit" (pd_lower, or less).  Returns false if it is malformed: its
 * header or payload does not fit before limit.
 */
bool
iheap_undo_read_record(Page page, uint32 off, uint32 limit,
					   IHeapUndoRecHeader *hdr, const char **payload,
					   uint32 *payload_len)
{
	IHeapUndoPageHeaderData *uph = IHeapUndoPageGetHeader(page);
	uint32		hdrlen;

	if (off < IHEAP_UNDO_DATA_START || off >= limit ||
		limit > BLCKSZ - IHEAP_SPECIAL_SIZE)
		return false;

	if (uph->uph_format == IHEAP_UNDO_FORMAT_V1)
	{
		if (off + IHEAP_UNDOREC_V1_HDRSZ > limit)
			return false;
		memcpy(hdr, (char *) page + off, IHEAP_UNDOREC_V1_HDRSZ);
		if (hdr->ur_len < IHEAP_UNDOREC_V1_HDRSZ || off + hdr->ur_len > limit)
			return false;
		hdrlen = IHEAP_UNDOREC_V1_HDRSZ;
		*payload_len = hdr->ur_len - IHEAP_UNDOREC_V1_HDRSZ;
	}
	else if (uph->uph_format == IHEAP_UNDO_FORMAT_COMPACT)
	{
		if (!undo_decode((char *) page + off, limit - off, uph->uph_lpage, hdr,
						 &hdrlen, payload_len))
			return false;
	}
	else
		return false;

	*payload = (char *) page + off + hdrlen;
	return true;
}

/*
 * Iterate over the records of an undo page: decodes the record at *off and
 * advances *off past it.  Returns false at the end of the records, or at a
 * malformed record (then *off is short of pd_lower).
 */
bool
iheap_undo_page_next(Page page, uint32 *off, IHeapUndoRecHeader *hdr,
					 const char **payload, uint32 *payload_len)
{
	uint32		lower = ((PageHeader) page)->pd_lower;

	if (*off >= lower ||
		!iheap_undo_read_record(page, *off, lower, hdr, payload, payload_len))
		return false;
	*off += hdr->ur_len;
	return true;
}

/*
 * Account a record for the page's xid range.  For LOCK records the tuple's
 * creator (prev header xid) is accounted too: readers may consult CLOG for
 * it while the LOCK record is live (relfrozenxid must not pass it), unless
 * the version was frozen when the record was written (its undo pointer is
 * then 0, and nobody looks its creator up).  So is every xid of a lockers
 * list: discard must keep the record while any of those lockers may still
 * be running, whoever wrote the record.
 */
static void
undo_page_account(Page page, const IHeapUndoRecHeader *hdr,
				  const char *payload, uint32 payload_len)
{
	IHeapUndoPageHeaderData *uph = IHeapUndoPageGetHeader(page);
	TransactionId xids[2 + IHEAP_MAX_LOCKERS];
	int			nxids = 0;

	xids[nxids++] = hdr->ur_xid;
	if (hdr->ur_type == IHU_LOCK)
	{
		IHeapHdr	ph;

		ih_hdr_read(hdr->ur_prev_hdr, &ph);
		if (TransactionIdIsNormal(ph.xid) && UndoRecPtrIsValid(ph.urec))
			xids[nxids++] = ph.xid;
	}
	if ((hdr->ur_flags & IHUF_LOCKERS) && payload_len >= sizeof(uint16))
	{
		uint16		n;

		memcpy(&n, payload + payload_len - sizeof(uint16), sizeof(uint16));
		if (n <= IHEAP_MAX_LOCKERS && iheap_lockers_size(n) <= payload_len)
		{
			const char *p = payload + payload_len - iheap_lockers_size(n);

			for (int i = 0; i < n; i++)
			{
				memcpy(&xids[nxids++], p, sizeof(TransactionId));
				p += IHEAP_LOCKER_SIZE;
			}
		}
	}

	for (int i = 0; i < nxids; i++)
	{
		TransactionId x = xids[i];

		if (!TransactionIdIsNormal(x))
			continue;
		if (!TransactionIdIsValid(uph->uph_min_xid) ||
			TransactionIdPrecedes(x, uph->uph_min_xid))
			uph->uph_min_xid = x;
		if (!TransactionIdIsValid(uph->uph_max_xid) ||
			TransactionIdFollows(x, uph->uph_max_xid))
			uph->uph_max_xid = x;
	}
	uph->uph_nrecs++;
}

/*
 * Append a record to an undo page, in the page's format (also used by
 * redo).  Sets hdr->ur_len and returns it.
 */
uint32
iheap_undo_put_record(Page page, uint32 offset, IHeapUndoRecHeader *hdr,
					  const char *payload, uint32 payload_len)
{
	IHeapUndoPageHeaderData *uph = IHeapUndoPageGetHeader(page);
	PageHeader	ph = (PageHeader) page;
	char		hbuf[IHEAP_UNDOREC_HDRSZ];
	uint32		hdrlen;
	uint32		len;

	if (uph->uph_format == IHEAP_UNDO_FORMAT_COMPACT)
		hdrlen = undo_encode(hbuf, hdr, payload_len, uph->uph_lpage);
	else
	{
		Assert(uph->uph_format == IHEAP_UNDO_FORMAT_V1);
		hdrlen = IHEAP_UNDOREC_V1_HDRSZ;
	}
	len = hdrlen + payload_len;
	if (offset != ph->pd_lower || offset + len > ph->pd_upper ||
		len > PG_UINT16_MAX)
		elog(PANIC, "iheap: bad undo record placement at %u (lower %u, upper %u, len %u)",
			 offset, ph->pd_lower, ph->pd_upper, len);
	hdr->ur_len = (uint16) len;
	if (uph->uph_format == IHEAP_UNDO_FORMAT_V1)
		memcpy(hbuf, hdr, IHEAP_UNDOREC_V1_HDRSZ);

	memcpy((char *) page + offset, hbuf, hdrlen);
	if (payload_len > 0)
		memcpy((char *) page + offset + hdrlen, payload, payload_len);
	undo_page_account(page, hdr, payload, payload_len);
	ph->pd_lower = offset + len;
	return len;
}

/*
 * Allocate and initialize a new undo page for "rel"; returns it pinned and
 * exclusively locked, with *lpage set.
 */
static Buffer
undo_alloc_page(Relation rel, uint64 *lpage)
{
	Buffer		metabuf;
	Page		metapage;
	IHeapMetaPageData *meta;
	IHeapExtent *ext;
	uint64		L;
	BlockNumber pblk;
	Buffer		undobuf;
	Page		undopage;

	metabuf = ReadBufferExtended(rel, MAIN_FORKNUM, IHEAP_METAPAGE_BLKNO,
								 RBM_NORMAL, NULL);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	meta = IHeapPageGetMeta(metapage);

	L = meta->im_next_lpage;
	ext = iheap_meta_find_extent(meta, L);
	if (ext == NULL)
	{
		IHeapExtent newext;
		int			freeidx = -1;

		for (uint32 i = 0; i < meta->im_nextents; i++)
		{
			if (meta->im_extents[i].ie_lstart == IHEAP_EXTENT_FREE)
			{
				freeidx = i;
				break;
			}
		}

		if (freeidx >= 0)
		{
			newext = meta->im_extents[freeidx];
			newext.ie_lstart = L;
			/* keep the array ordered by logical start: move it to the end */
			for (uint32 i = freeidx; i + 1 < meta->im_nextents; i++)
				meta->im_extents[i] = meta->im_extents[i + 1];
			meta->im_extents[meta->im_nextents - 1] = newext;
		}
		else
		{
			uint32		npages = iheap_meta_next_extent_size(meta);
			BlockNumber first = InvalidBlockNumber;
			uint32		done = 0;
			Buffer		bufs[64];

			if (meta->im_nextents >= (uint32) IHEAP_MAX_EXTENTS)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("too many undo extents in relation \"%s\"",
								RelationGetRelationName(rel)),
						 errhint("A long-running transaction is probably holding back undo discard.")));

			/*
			 * Extend by a contiguous run: hold the extension lock ourselves
			 * so nobody else extends in between.
			 */
			LockRelationForExtension(rel, ExclusiveLock);
			while (done < npages)
			{
				uint32		chunk = Min(npages - done, lengthof(bufs));
				uint32		got = 0;
				BlockNumber b;

				b = ExtendBufferedRelBy(BMR_REL(rel), MAIN_FORKNUM, NULL,
										EB_SKIP_EXTENSION_LOCK,
										chunk, bufs, &got);
				if (first == InvalidBlockNumber)
					first = b;
				else if (b != first + done)
					elog(ERROR, "iheap: non-contiguous relation extension");
				for (uint32 i = 0; i < got; i++)
					ReleaseBuffer(bufs[i]);
				done += got;
			}
			UnlockRelationForExtension(rel, ExclusiveLock);

			newext.ie_lstart = L;
			newext.ie_pstart = first;
			newext.ie_npages = npages;
			meta->im_extents[meta->im_nextents++] = newext;
		}
		ext = iheap_meta_find_extent(meta, L);
		Assert(ext != NULL);
	}

	pblk = ext->ie_pstart + (BlockNumber) (L - ext->ie_lstart);
	undobuf = ReadBufferExtended(rel, MAIN_FORKNUM, pblk, RBM_ZERO_AND_LOCK,
								 NULL);
	undopage = BufferGetPage(undobuf);

	START_CRIT_SECTION();

	meta->im_next_lpage = L + 1;
	iheap_meta_set_lower(metapage);
	iheap_undo_init_page(undopage, L);
	MarkBufferDirty(metabuf);
	MarkBufferDirty(undobuf);

	if (RelationNeedsWAL(rel))
	{
		xl_iheap_undo_alloc xlrec;
		XLogRecPtr	recptr;

		xlrec.lpage = L;
		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfIHeapUndoAlloc);
		XLogRegisterBuffer(IH_BLK_META, metabuf, REGBUF_STANDARD);
		XLogRegisterBufData(IH_BLK_META, meta, iheap_meta_image_size(metapage));
		XLogRegisterBuffer(IH_BLK_NEWUNDO, undobuf,
						   REGBUF_WILL_INIT | REGBUF_STANDARD);
		recptr = XLogInsert(RM_IHEAP_ID, XLOG_IHEAP_UNDO_ALLOC);
		PageSetLSN(metapage, recptr);
		PageSetLSN(undopage, recptr);
	}

	END_CRIT_SECTION();

	iheap_relcache_from_meta(rel, metapage);
	UnlockReleaseBuffer(metabuf);

	*lpage = L;
	return undobuf;
}

/*
 * Get an undo page with room for "len" more bytes, pinned and exclusively
 * locked.  The caller may hold data page locks (lock order allows it).
 */
void
iheap_undo_reserve(Relation rel, Size len, IHeapUndoReservation *res)
{
	UndoAppendEnt *ent;

	if (len > IHEAP_UNDO_PAGE_CAPACITY)
		elog(ERROR, "iheap: undo record of %zu bytes is too large", len);

	ent = undo_append_entry(rel);
	res->rel = rel;
	res->init = false;
	res->nwal = 0;

	/* outside the critical section of the writes (it may read the metapage) */
	res->discard = iheap_discard_ptr(rel);

	if (ent->lpage != 0 &&
		ent->blkno < RelationGetNumberOfBlocks(rel))
	{
		Buffer		buf;
		Page		page;

		buf = ReadBufferExtended(rel, MAIN_FORKNUM, ent->blkno, RBM_NORMAL,
								 NULL);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		if (IHeapPageGetType(page) == IHEAP_PAGE_UNDO &&
			IHeapUndoPageGetHeader(page)->uph_lpage == ent->lpage &&
			IHeapUndoPageGetHeader(page)->uph_format == IHEAP_UNDO_FORMAT_COMPACT &&
			(IHeapPageGetOpaque(page)->ihp_flags & IHU_DISCARDED) == 0 &&
			((PageHeader) page)->pd_upper - ((PageHeader) page)->pd_lower >= len)
		{
			res->buffer = buf;
			res->lpage = ent->lpage;
			res->offset = ((PageHeader) page)->pd_lower;
			return;
		}
		UnlockReleaseBuffer(buf);
	}

	res->buffer = undo_alloc_page(rel, &res->lpage);
	res->offset = ((PageHeader) BufferGetPage(res->buffer))->pd_lower;
	ent = undo_append_entry(rel);	/* hash may have moved; re-find */
	ent->lpage = res->lpage;
	ent->blkno = BufferGetBlockNumber(res->buffer);
	ent->nalloc++;
}

/*
 * Write a record into the reserved page.  Must be called inside the
 * caller's critical section; registers nothing in WAL by itself (see
 * iheap_undo_register_wal).  A previous version that was frozen when the
 * undo was reserved is recorded with undo pointer 0 (hdr is changed): it
 * needs no undo, and the compact header leaves the pointer out.
 */
UndoRecPtr
iheap_undo_write(Relation rel, IHeapUndoReservation *res,
				 IHeapUndoRecHeader *hdr, const char *payload,
				 uint32 payload_len)
{
	Page		page = BufferGetPage(res->buffer);
	uint32		off = res->offset;
	uint32		len;
	UndoRecPtr	urp;
	IHeapHdr	prev;

	Assert(CritSectionCount > 0 || !RelationNeedsWAL(rel));

	ih_hdr_read(hdr->ur_prev_hdr, &prev);
	if (UndoRecPtrIsValid(prev.urec) && prev.urec < res->discard)
	{
		prev.urec = InvalidUndoRecPtr;
		ih_hdr_write(hdr->ur_prev_hdr, &prev);
	}

	len = iheap_undo_put_record(page, off, hdr, payload, payload_len);
	res->offset = off + len;
	MarkBufferDirty(res->buffer);

	urp = MakeUndoRecPtr(res->lpage, off);

	if (res->nwal < lengthof(res->wal))
	{
		res->wal[res->nwal].offset = (uint16) off;
		res->wal[res->nwal].len = (uint16) len;
		res->nwal++;
	}
	else
		elog(PANIC, "iheap: too many undo records in one operation");

	return urp;
}

/*
 * Register the undo page and the records written so far in the WAL record
 * being assembled, in the compact form described in iheap.h.  datablk is the
 * block registered as IH_BLK_DATA, or InvalidBlockNumber.
 */
void
iheap_undo_register_wal(IHeapUndoReservation *res, uint8 block_id,
						BlockNumber datablk)
{
	Page		page = BufferGetPage(res->buffer);
	TransactionId recxid = GetCurrentTransactionIdIfAny();

	XLogRegisterBuffer(block_id, res->buffer, REGBUF_STANDARD);
	for (int i = 0; i < res->nwal; i++)
	{
		const char *payload;
		uint32		plen;
		uint16		payload_len;
		IHeapUndoRecHeader hdr;
		char	   *p = res->walhdr[i];
		uint8	   *wflags = (uint8 *) p;

		if (!iheap_undo_read_record(page, res->wal[i].offset,
									res->wal[i].offset + res->wal[i].len,
									&hdr, &payload, &plen))
			elog(PANIC, "iheap: undo record just written cannot be read");
		payload_len = (uint16) plen;
		*wflags = 0;
		p[1] = (char) hdr.ur_type;
		p[2] = (char) hdr.ur_flags;
		p += 3;
		memcpy(p, &payload_len, sizeof(uint16));
		p += sizeof(uint16);
		memcpy(p, &hdr.ur_cid, sizeof(CommandId));
		p += sizeof(CommandId);
		memcpy(p, &hdr.ur_offnum, sizeof(OffsetNumber));
		p += sizeof(OffsetNumber);
		if (!TransactionIdEquals(hdr.ur_xid, recxid))
		{
			*wflags |= IHUW_XID;
			memcpy(p, &hdr.ur_xid, sizeof(TransactionId));
			p += sizeof(TransactionId);
		}
		if (hdr.ur_blkno != datablk || !BlockNumberIsValid(datablk))
		{
			*wflags |= IHUW_BLKNO;
			memcpy(p, &hdr.ur_blkno, sizeof(BlockNumber));
			p += sizeof(BlockNumber);
		}
		if (!UndoRecPtrIsValid(hdr.ur_xact_prev))
			*wflags |= IHUW_PREV_NONE;
		else if (UndoRecPtrGetLPage(hdr.ur_xact_prev) == res->lpage)
		{
			uint16		off = (uint16) UndoRecPtrGetOffset(hdr.ur_xact_prev);

			*wflags |= IHUW_PREV_LOCAL;
			memcpy(p, &off, sizeof(uint16));
			p += sizeof(uint16);
		}
		else
		{
			memcpy(p, &hdr.ur_xact_prev, sizeof(UndoRecPtr));
			p += sizeof(UndoRecPtr);
		}
		memcpy(p, hdr.ur_prev_hdr, IHEAP_TUPHDR_SIZE);
		p += IHEAP_TUPHDR_SIZE;

		XLogRegisterBufData(block_id, res->walhdr[i], p - res->walhdr[i]);
		if (payload_len > 0)
			XLogRegisterBufData(block_id, payload, payload_len);
	}
}

/*
 * Redo: append the undo records carried in compact form by "record" to the
 * undo page.
 */
void
iheap_undo_redo_records(XLogReaderState *record, Page page, const char *data,
						Size len)
{
	const char *end = data + len;
	uint64		lpage = IHeapUndoPageGetHeader(page)->uph_lpage;

	while (data < end)
	{
		IHeapUndoRecHeader hdr;
		uint8		wflags = (uint8) data[0];
		uint16		payload_len;

		memset(&hdr, 0, sizeof(hdr));
		hdr.ur_type = (uint8) data[1];
		hdr.ur_flags = (uint8) data[2];
		data += 3;
		memcpy(&payload_len, data, sizeof(uint16));
		data += sizeof(uint16);
		memcpy(&hdr.ur_cid, data, sizeof(CommandId));
		data += sizeof(CommandId);
		memcpy(&hdr.ur_offnum, data, sizeof(OffsetNumber));
		data += sizeof(OffsetNumber);
		if (wflags & IHUW_XID)
		{
			memcpy(&hdr.ur_xid, data, sizeof(TransactionId));
			data += sizeof(TransactionId);
		}
		else
			hdr.ur_xid = XLogRecGetXid(record);
		if (wflags & IHUW_BLKNO)
		{
			memcpy(&hdr.ur_blkno, data, sizeof(BlockNumber));
			data += sizeof(BlockNumber);
		}
		else if (!XLogRecGetBlockTagExtended(record, IH_BLK_DATA, NULL, NULL,
											 &hdr.ur_blkno, NULL))
			elog(PANIC, "iheap: undo record refers to a missing data block");
		if (wflags & IHUW_PREV_NONE)
			hdr.ur_xact_prev = InvalidUndoRecPtr;
		else if (wflags & IHUW_PREV_LOCAL)
		{
			uint16		off;

			memcpy(&off, data, sizeof(uint16));
			data += sizeof(uint16);
			hdr.ur_xact_prev = MakeUndoRecPtr(lpage, off);
		}
		else
		{
			memcpy(&hdr.ur_xact_prev, data, sizeof(UndoRecPtr));
			data += sizeof(UndoRecPtr);
		}
		memcpy(hdr.ur_prev_hdr, data, IHEAP_TUPHDR_SIZE);
		data += IHEAP_TUPHDR_SIZE;

		if (data + payload_len > end)
			elog(PANIC, "iheap: corrupt undo data in WAL record");
		(void) iheap_undo_put_record(page, ((PageHeader) page)->pd_lower, &hdr,
									 data, payload_len);
		data += payload_len;
	}
}

void
iheap_undo_set_lsn(IHeapUndoReservation *res, XLogRecPtr lsn)
{
	PageSetLSN(BufferGetPage(res->buffer), lsn);
}

/* release the undo page; the records written are counted now */
void
iheap_undo_release(IHeapUndoReservation *res)
{
	if (BufferIsValid(res->buffer))
		UnlockReleaseBuffer(res->buffer);
	res->buffer = InvalidBuffer;

	if (res->nwal > 0 && res->rel != NULL)
	{
		uint64		bytes = 0;

		for (int i = 0; i < res->nwal; i++)
			bytes += res->wal[i].len;
		iheap_stat_count_undo(res->rel, res->nwal, bytes);
		res->nwal = 0;
	}
}

/*
 * Fetch a copy of the record at "urp".  Returns NULL if the record is no
 * longer there (discarded, page reused) and missing_ok; errors otherwise.
 * The caller may hold a data page lock.
 */
IHeapUndoRec *
iheap_undo_fetch(Relation rel, UndoRecPtr urp, bool missing_ok)
{
	uint64		lpage = UndoRecPtrGetLPage(urp);
	uint32		off = UndoRecPtrGetOffset(urp);
	BlockNumber blk;
	bool		found;
	Buffer		buf;
	Page		page;
	IHeapUndoRecHeader hdr;
	IHeapUndoRec *rec;
	PageHeader	ph;
	const char *payload;
	uint32		payload_len;

	/* 0: the version was frozen when its successor's record was written */
	if (!UndoRecPtrIsValid(urp))
	{
		if (missing_ok)
			return NULL;
		elog(ERROR, "iheap: invalid undo pointer");
	}

	blk = iheap_lpage_to_block(rel, lpage, &found);
	if (!found || blk >= RelationGetNumberOfBlocks(rel))
	{
		if (missing_ok)
			return NULL;
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("iheap: undo record " UINT64_FORMAT " of relation \"%s\" is not available",
						urp, RelationGetRelationName(rel))));
	}

	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	ph = (PageHeader) page;

	if (IHeapPageGetType(page) != IHEAP_PAGE_UNDO ||
		IHeapUndoPageGetHeader(page)->uph_lpage != lpage ||
		off < IHEAP_UNDO_DATA_START ||
		off >= ph->pd_lower)
	{
		UnlockReleaseBuffer(buf);
		if (missing_ok)
			return NULL;
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("iheap: undo record " UINT64_FORMAT " of relation \"%s\" not found",
						urp, RelationGetRelationName(rel))));
	}

	if (!iheap_undo_read_record(page, off, ph->pd_lower, &hdr, &payload,
								&payload_len))
	{
		UnlockReleaseBuffer(buf);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("iheap: undo record " UINT64_FORMAT " of relation \"%s\" is corrupt",
						urp, RelationGetRelationName(rel))));
	}

	/* the copy keeps the payload only; the header is decoded */
	rec = palloc(offsetof(IHeapUndoRec, data) + Max(payload_len, 1));
	rec->urp = urp;
	rec->hdr = hdr;
	memcpy(rec->data, payload, payload_len);
	rec->payload = rec->data;
	rec->payload_len = payload_len;

	UnlockReleaseBuffer(buf);
	return rec;
}

/*
 * Fetch the record that made the version with header h: the one h.urec
 * points to or, under a LOCK record, the one before it.  NULL if it is no
 * longer there.
 */
IHeapUndoRec *
iheap_undo_fetch_version_rec(Relation rel, const IHeapHdr *h)
{
	IHeapUndoRec *rec = iheap_undo_fetch(rel, h->urec, true);

	if (rec != NULL && rec->hdr.ur_type == IHU_LOCK)
	{
		IHeapHdr	ph;

		ih_hdr_read(rec->hdr.ur_prev_hdr, &ph);
		pfree(rec);
		rec = iheap_undo_fetch(rel, ph.urec, true);
	}
	return rec;
}

/*
 * The undo pointer that identifies the version with header h: that of the
 * record that made it (LOCK records on top do not make new versions).
 */
UndoRecPtr
iheap_version_urec(Relation rel, const IHeapHdr *h)
{
	UndoRecPtr	urp = h->urec;

	if ((h->infomask & IHT_LOCKED) && urp >= iheap_discard_ptr(rel))
	{
		IHeapUndoRec *rec = iheap_undo_fetch(rel, urp, true);

		if (rec != NULL && rec->hdr.ur_type == IHU_LOCK)
		{
			IHeapHdr	ph;

			ih_hdr_read(rec->hdr.ur_prev_hdr, &ph);
			urp = ph.urec;
		}
		if (rec)
			pfree(rec);
	}
	return urp;
}

/* ------------------------------------------------------------------------
 * Lockers lists
 *
 * Layout at the end of a record's payload:
 *		{ xid (4), mode (1) } * n, uint16 n
 * The count comes last so it can be found from the end of the payload.
 * ------------------------------------------------------------------------
 */
uint32
iheap_lockers_size(int nlockers)
{
	return nlockers * IHEAP_LOCKER_SIZE + sizeof(uint16);
}

void
iheap_lockers_serialize(char *dst, const IHeapLocker *lockers, int nlockers)
{
	uint16		n = (uint16) nlockers;

	for (int i = 0; i < nlockers; i++)
	{
		memcpy(dst, &lockers[i].xid, sizeof(TransactionId));
		dst[sizeof(TransactionId)] = (char) lockers[i].mode;
		dst += IHEAP_LOCKER_SIZE;
	}
	memcpy(dst, &n, sizeof(uint16));
}

/* read the lockers list of a record (0 if none) */
int
iheap_undo_lockers(const IHeapUndoRec *rec, IHeapLocker *lockers)
{
	uint16		n;
	const char *p;

	if ((rec->hdr.ur_flags & IHUF_LOCKERS) == 0 ||
		rec->payload_len < sizeof(uint16))
		return 0;

	memcpy(&n, rec->payload + rec->payload_len - sizeof(uint16), sizeof(uint16));
	if (n > IHEAP_MAX_LOCKERS ||
		iheap_lockers_size(n) > rec->payload_len)
		elog(ERROR, "iheap: corrupt lockers list in undo record " UINT64_FORMAT,
			 rec->urp);

	p = rec->payload + rec->payload_len - iheap_lockers_size(n);
	for (int i = 0; i < n; i++)
	{
		memcpy(&lockers[i].xid, p, sizeof(TransactionId));
		lockers[i].mode = (uint8) p[sizeof(TransactionId)];
		p += IHEAP_LOCKER_SIZE;
	}
	return n;
}

/* payload length excluding a trailing lockers list */
uint32
iheap_undo_payload_core_len(const IHeapUndoRec *rec)
{
	uint16		n;

	if ((rec->hdr.ur_flags & IHUF_LOCKERS) == 0)
		return rec->payload_len;
	memcpy(&n, rec->payload + rec->payload_len - sizeof(uint16), sizeof(uint16));
	return rec->payload_len - iheap_lockers_size(n);
}
