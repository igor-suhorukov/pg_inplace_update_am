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
	((PageHeader) page)->pd_lower = IHEAP_UNDO_DATA_START;
}

/*
 * Account a record for the page's xid range.  For LOCK records the tuple's
 * creator (prev header xid) is accounted too: readers may consult CLOG for
 * it while the LOCK record is live (relfrozenxid must not pass it).  So is
 * every xid of a lockers list: discard must keep the record while any of
 * those lockers may still be running, whoever wrote the record.
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
		if (TransactionIdIsNormal(ph.xid))
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

/* place a record on an undo page (also used by redo) */
void
iheap_undo_put_record(Page page, uint32 offset, const char *rec, uint32 len)
{
	IHeapUndoRecHeader hdr;
	PageHeader	ph = (PageHeader) page;

	if (offset != ph->pd_lower || offset + len > ph->pd_upper)
		elog(PANIC, "iheap: bad undo record placement at %u (lower %u, upper %u, len %u)",
			 offset, ph->pd_lower, ph->pd_upper, len);
	memcpy((char *) page + offset, rec, len);
	memcpy(&hdr, rec, IHEAP_UNDOREC_HDRSZ);
	undo_page_account(page, &hdr, rec + IHEAP_UNDOREC_HDRSZ,
					  len - IHEAP_UNDOREC_HDRSZ);
	ph->pd_lower = offset + len;
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
	res->init = false;
	res->nwal = 0;

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
 * iheap_undo_register_wal).
 */
UndoRecPtr
iheap_undo_write(Relation rel, IHeapUndoReservation *res,
				 IHeapUndoRecHeader *hdr, const char *payload,
				 uint32 payload_len)
{
	Page		page = BufferGetPage(res->buffer);
	uint32		len = IHEAP_UNDOREC_HDRSZ + payload_len;
	uint32		off = res->offset;
	PageHeader	ph = (PageHeader) page;
	UndoRecPtr	urp;

	Assert(CritSectionCount > 0 || !RelationNeedsWAL(rel));
	if (len > PG_UINT16_MAX || off + len > ph->pd_upper)
		elog(PANIC, "iheap: undo record does not fit (offset %u, len %u)", off, len);

	hdr->ur_len = (uint16) len;
	memcpy((char *) page + off, hdr, IHEAP_UNDOREC_HDRSZ);
	if (payload_len > 0)
		memcpy((char *) page + off + IHEAP_UNDOREC_HDRSZ, payload, payload_len);
	undo_page_account(page, hdr, payload, payload_len);
	ph->pd_lower = off + len;
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
		const char *rec = (char *) page + res->wal[i].offset;
		uint16		payload_len = res->wal[i].len - IHEAP_UNDOREC_HDRSZ;
		IHeapUndoRecHeader hdr;
		char	   *p = res->walhdr[i];
		uint8	   *wflags = (uint8 *) p;

		memcpy(&hdr, rec, IHEAP_UNDOREC_HDRSZ);
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
			XLogRegisterBufData(block_id, rec + IHEAP_UNDOREC_HDRSZ, payload_len);
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
	char		buf[BLCKSZ];

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
		hdr.ur_len = IHEAP_UNDOREC_HDRSZ + payload_len;

		if (data + payload_len > end || hdr.ur_len > sizeof(buf))
			elog(PANIC, "iheap: corrupt undo data in WAL record");
		memcpy(buf, &hdr, IHEAP_UNDOREC_HDRSZ);
		memcpy(buf + IHEAP_UNDOREC_HDRSZ, data, payload_len);
		data += payload_len;
		iheap_undo_put_record(page, ((PageHeader) page)->pd_lower, buf,
							  hdr.ur_len);
	}
}

void
iheap_undo_set_lsn(IHeapUndoReservation *res, XLogRecPtr lsn)
{
	PageSetLSN(BufferGetPage(res->buffer), lsn);
}

void
iheap_undo_release(IHeapUndoReservation *res)
{
	if (BufferIsValid(res->buffer))
		UnlockReleaseBuffer(res->buffer);
	res->buffer = InvalidBuffer;
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

	if (!UndoRecPtrIsValid(urp))
		elog(ERROR, "iheap: invalid undo pointer");

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
		off + IHEAP_UNDOREC_HDRSZ > ph->pd_lower)
	{
		UnlockReleaseBuffer(buf);
		if (missing_ok)
			return NULL;
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("iheap: undo record " UINT64_FORMAT " of relation \"%s\" not found",
						urp, RelationGetRelationName(rel))));
	}

	memcpy(&hdr, (char *) page + off, IHEAP_UNDOREC_HDRSZ);
	if (hdr.ur_len < IHEAP_UNDOREC_HDRSZ || off + hdr.ur_len > ph->pd_lower)
	{
		UnlockReleaseBuffer(buf);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("iheap: undo record " UINT64_FORMAT " of relation \"%s\" is corrupt",
						urp, RelationGetRelationName(rel))));
	}

	rec = palloc(offsetof(IHeapUndoRec, data) + hdr.ur_len);
	rec->urp = urp;
	rec->hdr = hdr;
	memcpy(rec->data, (char *) page + off, hdr.ur_len);
	rec->payload = rec->data + IHEAP_UNDOREC_HDRSZ;
	rec->payload_len = hdr.ur_len - IHEAP_UNDOREC_HDRSZ;

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
