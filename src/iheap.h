/*-------------------------------------------------------------------------
 *
 * iheap.h
 *	  Internal definitions of the iheap table access method: an in-place
 *	  update storage engine that keeps prior row versions in undo.
 *
 * See DESIGN.md (sections 5, 6 and 21) for the on-disk format and the
 * reasoning behind it.  In short:
 *
 *	- Block 0 of the main fork is a metapage.  The other blocks are data
 *	  pages and undo pages; their type is recorded in the page special area.
 *	- Every tuple has a 16-byte header holding the 32-bit xid of the
 *	  transaction that created the current version and a 56-bit logical
 *	  undo pointer to the record describing the previous version.
 *	- A relation's undo is one logical byte stream stored in extents of the
 *	  main fork.  Everything below the discard pointer has been verified
 *	  (aborted changes applied) and discarded, so a tuple whose undo pointer
 *	  is below it is "frozen": visible to everybody, no xid lookups needed.
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#ifndef IHEAP_H
#define IHEAP_H

#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/toast_helper.h"
#include "access/xlogreader.h"
#include "executor/tuptable.h"
#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"
#include "storage/read_stream.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

/* ------------------------------------------------------------------------
 * Page special area (all page types)
 * ------------------------------------------------------------------------
 */
typedef struct IHeapPageOpaqueData
{
	uint16		ihp_type;		/* IHEAP_PAGE_* */
	uint16		ihp_flags;		/* IHP_* (data pages) or IHU_* (undo pages) */
	uint32		ihp_reserved;
} IHeapPageOpaqueData;

typedef IHeapPageOpaqueData *IHeapPageOpaque;

#define IHEAP_SPECIAL_SIZE	MAXALIGN(sizeof(IHeapPageOpaqueData))

#define IHEAP_PAGE_META		0x6901
#define IHEAP_PAGE_DATA		0x6902
#define IHEAP_PAGE_UNDO		0x6903

/* data page flags */
#define IHP_PRUNABLE		0x0001	/* page may hold reclaimable space */

/* undo page flags */
#define IHU_DISCARDED		0x0001	/* hint: discarded, do not append */

#define IHEAP_METAPAGE_BLKNO	((BlockNumber) 0)

static inline IHeapPageOpaque
IHeapPageGetOpaque(Page page)
{
	return (IHeapPageOpaque) PageGetSpecialPointer(page);
}

static inline uint16
IHeapPageGetType(Page page)
{
	if (PageIsNew(page))
		return 0;
	if (((PageHeader) page)->pd_special != BLCKSZ - IHEAP_SPECIAL_SIZE)
		return 0;
	return IHeapPageGetOpaque(page)->ihp_type;
}

static inline bool
IHeapPageIsData(Page page)
{
	return IHeapPageGetType(page) == IHEAP_PAGE_DATA;
}

/*
 * TID bitmaps reject offsets above MaxHeapTuplesPerPage, so a data page
 * never holds more line pointers than heap would (DESIGN.md A8).
 */
#define IHEAP_MAX_ITEMS		MaxHeapTuplesPerPage

/* ------------------------------------------------------------------------
 * Undo pointers
 * ------------------------------------------------------------------------
 */

/*
 * Logical byte position in a relation's undo stream.  0 is never a valid
 * record position (logical page 0 is never allocated).
 */
typedef uint64 UndoRecPtr;

#define InvalidUndoRecPtr	((UndoRecPtr) 0)
#define UndoRecPtrIsValid(p)	((p) != InvalidUndoRecPtr)
#define IHEAP_UREC_BITS		56
#define UndoRecPtrGetLPage(p)	((uint64) ((p) / BLCKSZ))
#define UndoRecPtrGetOffset(p)	((uint32) ((p) % BLCKSZ))
#define MakeUndoRecPtr(lpage, off)	((UndoRecPtr) (lpage) * BLCKSZ + (off))

/* ------------------------------------------------------------------------
 * Tuple header
 *
 * Tuples start at 2-byte-aligned page offsets, so the multi-byte fields
 * other than the two uint16s are read and written with memcpy.
 *
 *	offset 0	uint16	infomask2	(natts | flags)
 *	offset 2	uint16	infomask	(flags)
 *	offset 4	uint8	hoff		(header + null bitmap length)
 *	offset 5	7 bytes	urec		(56-bit undo pointer, little endian)
 *	offset 12	uint32	xid			(creator of the current version)
 *
 * The null bitmap (if any) follows the header; the data area starts at
 * hoff.  Pass-by-value attributes are stored without alignment padding.
 * Pass-by-reference attributes are aligned relative to the data area; when
 * a tuple contains such a value, IHT_ALIGNDATA is set and the tuple is
 * placed so that its data area is MAXALIGNed in memory.
 * ------------------------------------------------------------------------
 */
#define IHEAP_TUPHDR_SIZE	16

#define IHT_NATTS_MASK		0x07FF	/* in infomask2 */

#define IHT_HASNULL			0x0001
#define IHT_HASVARWIDTH		0x0002
#define IHT_HASEXTERNAL		0x0004
#define IHT_ALIGNDATA		0x0008	/* data area must be MAXALIGNed */
#define IHT_XID_COMMITTED	0x0010	/* hint: xid committed */
#define IHT_XID_ABORTED		0x0020	/* hint: xid aborted */
#define IHT_DELETED			0x0040	/* version deleted by xid */
#define IHT_MOVED			0x0080	/* moved to another TID by xid */
#define IHT_LOCKED			0x0100	/* record at urec carries lockers */
#define IHT_INPLACE			0x0200	/* version made by an in-place update */
#define IHT_SPECULATIVE		0x0400	/* speculative insertion */
#define IHT_STUB			0x0800	/* body reclaimed, header only */
#define IHT_KEYS_UPDATED	0x1000	/* modification by xid changed keys */
#define IHT_MOVED_PART		0x2000	/* deleted by a move to another partition */

#define IHT_XID_HINTS		(IHT_XID_COMMITTED | IHT_XID_ABORTED)
#define IHT_DATA_FLAGS		(IHT_HASNULL | IHT_HASVARWIDTH | IHT_HASEXTERNAL | \
							 IHT_ALIGNDATA)

/* decoded header */
typedef struct IHeapHdr
{
	uint16		infomask2;
	uint16		infomask;
	uint8		hoff;
	UndoRecPtr	urec;
	TransactionId xid;
} IHeapHdr;

static inline void
ih_hdr_read(const char *tup, IHeapHdr *h)
{
	const uint8 *b = (const uint8 *) tup;
	uint64		u = 0;

	memcpy(&h->infomask2, tup, sizeof(uint16));
	memcpy(&h->infomask, tup + 2, sizeof(uint16));
	h->hoff = b[4];
	for (int i = 6; i >= 0; i--)
		u = (u << 8) | b[5 + i];
	h->urec = u;
	memcpy(&h->xid, tup + 12, sizeof(TransactionId));
}

static inline void
ih_hdr_write(char *tup, const IHeapHdr *h)
{
	uint8	   *b = (uint8 *) tup;
	uint64		u = h->urec;

	memcpy(tup, &h->infomask2, sizeof(uint16));
	memcpy(tup + 2, &h->infomask, sizeof(uint16));
	b[4] = h->hoff;
	for (int i = 0; i < 7; i++)
	{
		b[5 + i] = (uint8) (u & 0xFF);
		u >>= 8;
	}
	memcpy(tup + 12, &h->xid, sizeof(TransactionId));
}

static inline uint16
ih_hdr_infomask(const char *tup)
{
	uint16		v;

	memcpy(&v, tup + 2, sizeof(uint16));
	return v;
}

static inline void
ih_hdr_set_infomask(char *tup, uint16 v)
{
	memcpy(tup + 2, &v, sizeof(uint16));
}

static inline int
ih_hdr_natts(const char *tup)
{
	uint16		v;

	memcpy(&v, tup, sizeof(uint16));
	return v & IHT_NATTS_MASK;
}

static inline uint8
ih_hdr_hoff(const char *tup)
{
	return ((const uint8 *) tup)[4];
}

static inline bool
ih_tuple_needs_align(const char *tup)
{
	return (ih_hdr_infomask(tup) & IHT_ALIGNDATA) != 0;
}

/* ------------------------------------------------------------------------
 * Metapage
 * ------------------------------------------------------------------------
 */
typedef struct IHeapExtent
{
	uint64		ie_lstart;		/* first logical undo page, or IHEAP_EXTENT_FREE */
	BlockNumber ie_pstart;		/* first physical block */
	uint32		ie_npages;		/* number of pages */
} IHeapExtent;

#define IHEAP_EXTENT_FREE	PG_UINT64_MAX

typedef struct IHeapMetaPageData
{
	uint32		im_magic;
	uint32		im_version;
	UndoRecPtr	im_discard;		/* undo below this has been discarded */
	uint64		im_next_lpage;	/* next logical undo page to allocate */
	uint32		im_nextents;	/* used entries in im_extents */
	uint32		im_flags;
	uint32		im_undo_peak;	/* decaying high-water mark of the pages in
								 * live extents (iheap_meta_free_extents) */
	uint32		im_pad;
	IHeapExtent im_extents[FLEXIBLE_ARRAY_MEMBER];
} IHeapMetaPageData;

#define IHEAP_META_MAGIC	0x69486561	/* "iHea" */
#define IHEAP_META_VERSION	2

#define IHEAP_MAX_EXTENTS \
	((int) ((BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - IHEAP_SPECIAL_SIZE - \
			 offsetof(IHeapMetaPageData, im_extents)) / sizeof(IHeapExtent)))

#define IHEAP_EXTENT_MIN_PAGES	4
#define IHEAP_EXTENT_MAX_PAGES	1024
#define IHEAP_FREE_EXTENT_RESERVE	2

static inline IHeapMetaPageData *
IHeapPageGetMeta(Page page)
{
	return (IHeapMetaPageData *) PageGetContents(page);
}

/* ------------------------------------------------------------------------
 * Undo pages and records
 * ------------------------------------------------------------------------
 */
typedef struct IHeapUndoPageHeaderData
{
	uint64		uph_lpage;		/* logical undo page number */
	TransactionId uph_min_xid;	/* oldest xid referenced by records */
	TransactionId uph_max_xid;	/* newest record xid */
	uint16		uph_nrecs;
	uint16		uph_reserved;
	uint32		uph_reserved2;
} IHeapUndoPageHeaderData;

#define IHEAP_UNDO_DATA_START \
	(MAXALIGN(SizeOfPageHeaderData) + MAXALIGN(sizeof(IHeapUndoPageHeaderData)))
#define IHEAP_UNDO_PAGE_CAPACITY \
	(BLCKSZ - IHEAP_SPECIAL_SIZE - IHEAP_UNDO_DATA_START)

static inline IHeapUndoPageHeaderData *
IHeapUndoPageGetHeader(Page page)
{
	return (IHeapUndoPageHeaderData *) PageGetContents(page);
}

/* record types */
#define IHU_INSERT			1	/* payload: [token][lockers] */
#define IHU_MULTI_INSERT	2	/* payload: uint16 n, OffsetNumber[n] */
#define IHU_DELETE			3	/* payload: old body */
#define IHU_UPDATE			4	/* in-place; payload: delta [lockers] */
#define IHU_MOVE			5	/* old TID of a moved row; payload: tid, old body */
#define IHU_LOCK			6	/* payload: lockers */

/* record flags */
#define IHUF_SPEC			0x01	/* INSERT carries a speculative token */
#define IHUF_LOCKERS		0x02	/* record ends with a lockers list */
#define IHUF_KEYS_UPDATED	0x04	/* UPDATE/MOVE changed key columns */
#define IHUF_MOVED_IN		0x08	/* INSERT is the new home of a moved row */

/*
 * Undo record header, stored unaligned (read and written with memcpy).
 * ur_prev_hdr is the target item's tuple header before the operation (all
 * zeroes for inserts); walking a version chain means taking ur_prev_hdr as
 * the header of the previous version.
 */
typedef struct IHeapUndoRecHeader
{
	UndoRecPtr	ur_xact_prev;	/* previous record of the same top-level
								 * transaction in this relation */
	TransactionId ur_xid;		/* writer ((sub)transaction) */
	CommandId	ur_cid;
	BlockNumber ur_blkno;
	OffsetNumber ur_offnum;
	uint16		ur_len;			/* total length including header */
	uint8		ur_type;
	uint8		ur_flags;
	char		ur_prev_hdr[IHEAP_TUPHDR_SIZE];
} IHeapUndoRecHeader;

#define IHEAP_UNDOREC_HDRSZ	(offsetof(IHeapUndoRecHeader, ur_prev_hdr) + IHEAP_TUPHDR_SIZE)

/* lockers carried by INSERT/UPDATE/LOCK records */
typedef struct IHeapLocker
{
	TransactionId xid;
	uint8		mode;			/* LockTupleMode */
} IHeapLocker;

#define IHEAP_LOCKER_SIZE	(sizeof(TransactionId) + 1)
#define IHEAP_MAX_LOCKERS	64

/* in-memory copy of a record */
typedef struct IHeapUndoRec
{
	UndoRecPtr	urp;			/* where it lives */
	IHeapUndoRecHeader hdr;
	char	   *payload;		/* points into data */
	uint32		payload_len;
	char		data[FLEXIBLE_ARRAY_MEMBER];
} IHeapUndoRec;

/*
 * Largest tuple on a data page: the undo records of any operation on it
 * (a MOVE record with the whole old body plus an INSERT record with
 * lockers) must fit on one undo page.
 */
#define IHEAP_MAX_TUPLE_SIZE \
	(IHEAP_UNDO_PAGE_CAPACITY - 2 * IHEAP_UNDOREC_HDRSZ - \
	 sizeof(ItemPointerData) - (IHEAP_MAX_LOCKERS * IHEAP_LOCKER_SIZE + 2) - 64)

/* in-place update delta header (payload of IHU_UPDATE) */
typedef struct IHeapUpdateDelta
{
	uint16		old_len;		/* old tuple length (header + body) */
	uint16		prefix;			/* bytes shared with new body at the start */
	uint16		suffix;			/* bytes shared with new body at the end */
	uint16		mid_len;		/* old bytes between prefix and suffix */
} IHeapUpdateDelta;

/* ------------------------------------------------------------------------
 * Relcache-attached cache (rel->rd_amcache): metapage snapshot
 * ------------------------------------------------------------------------
 */
typedef struct IHeapRelCache
{
	uint32		rc_magic;
	UndoRecPtr	rc_discard;
	uint64		rc_next_lpage;
	TimestampTz rc_refreshed;
	uint32		rc_extent_pages;	/* pages in extents, live or free */
	int			rc_nall;		/* extents, live or free ... */
	IHeapExtent *rc_all;		/* ... sorted by physical start */
	int			rc_nlive;
	IHeapExtent rc_live[FLEXIBLE_ARRAY_MEMBER];	/* sorted by logical start;
												 * rc_all follows */
} IHeapRelCache;

#define IHEAP_RELCACHE_MAGIC 0x69686331

/* ------------------------------------------------------------------------
 * Tuple table slot
 * ------------------------------------------------------------------------
 */
typedef struct IHeapTupleTableSlot
{
	TupleTableSlot base;
	char	   *tuple;			/* header + body; data area aligned if needed */
	uint32		tuplen;
	uint32		off;			/* deform: offset into data area */
	char	   *mem;			/* allocation owning *tuple, if TTS_SHOULDFREE */
	TransactionId xmin;			/* creator of this version */
	TransactionId xmax;			/* deleter/updater of this version, if known */
	CommandId	cmin;			/* creator's command id, if current xact */
} IHeapTupleTableSlot;

extern PGDLLIMPORT const TupleTableSlotOps TTSOpsIHeapTuple;

#define TTS_IS_IHEAPTUPLE(slot) ((slot)->tts_ops == &TTSOpsIHeapTuple)

/* ------------------------------------------------------------------------
 * A version of a row as seen by some snapshot
 * ------------------------------------------------------------------------
 */
typedef struct IHeapVersion
{
	char	   *tuple;			/* header + body */
	uint32		len;
	char	   *mem;			/* allocation holding tuple, if from undo */
	bool		from_undo;		/* rebuilt from undo (palloc'd in caller ctx) */
	TransactionId xmin;			/* creator of this version */
	TransactionId xmax;			/* who deleted/updated it (if visible state needs it) */
	CommandId	cmin;
} IHeapVersion;

/* ------------------------------------------------------------------------
 * Scan descriptors
 * ------------------------------------------------------------------------
 */
typedef struct IHeapScanDescData
{
	TableScanDescData rs_base;

	BlockNumber rs_nblocks;		/* number of blocks to scan */
	BlockNumber rs_startblock;
	BlockNumber rs_numblocks;	/* max blocks, or InvalidBlockNumber */
	BlockNumber rs_cblock;		/* current block */
	BlockNumber rs_prefetch_block;
	bool		rs_inited;
	ScanDirection rs_dir;
	BufferAccessStrategy rs_strategy;
	ReadStream *rs_read_stream;
	ParallelBlockTableScanWorkerData *rs_parallelworkerdata;

	/* blocks that hold no data: metapage and undo extents (by start) */
	IHeapExtent *rs_skip;
	int			rs_nskip;

	/* page-at-a-time state */
	char	   *rs_page;		/* private copy of current data page */
	int			rs_ntuples;		/* visible versions on current page */
	int			rs_cindex;		/* current index into rs_vis */
	OffsetNumber rs_visoff[IHEAP_MAX_ITEMS];
	IHeapVersion rs_vis[IHEAP_MAX_ITEMS];
	MemoryContext rs_pagecxt;	/* versions rebuilt from undo */

	/* TID range scans */
	ItemPointerData rs_mintid;
	ItemPointerData rs_maxtid;

	/* bitmap scans */
	bool		rs_bm_recheck;

	/* sample scans */
	BlockNumber rs_sample_block;
	bool		rs_sample_inited;
	OffsetNumber rs_sample_maxoff;

	/* analyze */
	OffsetNumber rs_analyze_off;
	Buffer		rs_analyze_buf;
} IHeapScanDescData;

typedef IHeapScanDescData *IHeapScanDesc;

/*
 * The dmbtree entry an index fetch comes from (DESIGN.md §11.5.4): the row
 * version found is checked against its key when the entry is delete-marked
 * or the version was rebuilt from undo, or when the snapshot is not an MVCC
 * one and the entry may have been marked since it was read.
 */
typedef struct IHeapDmEntry
{
	Relation	index;			/* NULL: no entry to check against */
	IndexTuple	itup;
	bool		marked;
	bool		locked;			/* read under the leaf page lock, still held */
} IHeapDmEntry;

typedef struct IHeapIndexFetchData
{
	IndexFetchTableData xs_base;
	Buffer		xs_cbuf;		/* pinned buffer of last fetch */
	IHeapDmEntry xs_dm;			/* set by dmbtree for each entry it returns */
} IHeapIndexFetchData;

/*
 * Indexes whose key columns an in-place update may change (dmbtree indexes
 * that can carry delete-marks), and the columns whose change makes an
 * update move the row instead (§7.4).  Cached per relation, rebuilt after
 * a relcache invalidation of the table or of one of its indexes.
 */
typedef struct IHeapDmIndex
{
	Oid			indexoid;
	int			natts;			/* key and INCLUDE columns */
	AttrNumber	attnums[INDEX_MAX_KEYS];	/* table column of each */
	Bitmapset  *keyattrs;		/* key columns, as in the bitmaps below */
	bool		unique;
} IHeapDmIndex;

typedef struct IHeapDmInfo
{
	Oid			relid;			/* hash key */
	bool		valid;
	MemoryContext cxt;			/* holds the rest */
	Bitmapset  *blocking;		/* columns (offset by
								 * FirstLowInvalidHeapAttributeNumber) whose
								 * change forces a move */
	Bitmapset  *dmattrs;		/* key columns of the indexes below */
	int			ndm;
	IHeapDmIndex *dm;
} IHeapDmInfo;

/* ------------------------------------------------------------------------
 * Function prototypes
 * ------------------------------------------------------------------------
 */

/* iheap_handler.c */
extern Relation iheap_fake_relation_open(RelFileLocator rlocator,
										 char persistence);
extern void iheap_fake_relation_close(Relation rel);
extern bool iheap_enable_inplace;
extern int	iheap_foreground_undo_limit;
extern int	iheap_logical_decoding;

/* iheap_tuple.c */
extern Size ih_compute_data_size(TupleDesc desc, const Datum *values,
								 const bool *isnull, bool *needs_align);
extern char *ih_form_tuple(TupleDesc desc, const Datum *values,
						   const bool *isnull, uint32 *len, char **mem);
extern void ih_deform_tuple(TupleDesc desc, const char *tuple,
							Datum *values, bool *isnull);
extern void ih_deform_into_slot(TupleTableSlot *slot, const char *tuple,
								uint32 *offp, int natts);
extern char *ih_copy_tuple_aligned(MemoryContext cxt, const char *tuple,
								   uint32 len, char **mem);
extern uint32 ih_tuple_len_on_page(Page page, OffsetNumber off);

/* iheap_slot.c */
extern void iheap_store_tuple(TupleTableSlot *slot, char *tuple, uint32 len,
							  bool copy, TransactionId xmin,
							  TransactionId xmax, CommandId cmin);
extern void iheap_store_version(TupleTableSlot *slot, IHeapVersion *v,
								bool copy);

/* iheap_meta.c */
extern void iheap_init_metapage(Page page);
extern void iheap_init_data_page(Page page);
extern IHeapRelCache *iheap_relcache(Relation rel, bool refresh);
extern UndoRecPtr iheap_discard_ptr(Relation rel);
extern BlockNumber iheap_lpage_to_block(Relation rel, uint64 lpage, bool *found);
extern void iheap_relcache_invalidate(Relation rel);
extern int	iheap_nondata_ranges(Relation rel, IHeapExtent **ranges);
extern uint32 iheap_extent_pages(Relation rel);

/* iheap_meta.c (metapage manipulation) */
extern void iheap_meta_set_lower(Page page);
extern Size iheap_meta_image_size(Page page);
extern void iheap_relcache_from_meta(Relation rel, Page metapage);
extern IHeapExtent *iheap_meta_find_extent(IHeapMetaPageData *meta,
										   uint64 lpage);
extern uint64 iheap_meta_live_pages(IHeapMetaPageData *meta);
extern int	iheap_meta_free_extents(IHeapMetaPageData *meta,
									IHeapExtent *released, int maxreleased,
									uint64 passed);
extern uint32 iheap_meta_next_extent_size(IHeapMetaPageData *meta);
extern bool iheap_meta_block_in_extent(IHeapMetaPageData *meta,
									   BlockNumber blk);

/* iheap_undo.c */
typedef struct IHeapUndoWalEnt
{
	uint16		offset;
	uint16		len;
} IHeapUndoWalEnt;

/*
 * Undo records travel in WAL as buffer data of their undo page, each as a
 * compact header followed by the payload.  The header leaves out what redo
 * can derive (DESIGN.md §21 D15): the placement (records are appended at
 * pd_lower), the length, the writer when it is the WAL record's xid, the
 * target block when it is the record's data block (IH_BLK_DATA), and the
 * logical page of the chain pointer when it is the undo page's own.
 *
 *		wflags (1), ur_type (1), ur_flags (1), payload length (2),
 *		ur_cid (4), ur_offnum (2), [ur_xid (4)], [ur_blkno (4)],
 *		ur_xact_prev (0, 2 or 8), ur_prev_hdr (16)
 */
#define IHUW_XID			0x01	/* ur_xid present */
#define IHUW_BLKNO			0x02	/* ur_blkno present */
#define IHUW_PREV_LOCAL		0x04	/* ur_xact_prev: offset on this page */
#define IHUW_PREV_NONE		0x08	/* ur_xact_prev invalid */
#define IHEAP_UNDOWAL_MAXHDR	(1 + 1 + 1 + 2 + 4 + 2 + 4 + 4 + 8 + IHEAP_TUPHDR_SIZE)

typedef struct IHeapUndoReservation
{
	Buffer		buffer;			/* locked undo page */
	uint64		lpage;
	bool		init;			/* page initialized by this operation */
	uint32		offset;			/* where the next record goes */
	int			nwal;			/* records written, for WAL registration */
	IHeapUndoWalEnt wal[4];
	char		walhdr[4][IHEAP_UNDOWAL_MAXHDR];	/* their compact headers */
} IHeapUndoReservation;

extern void iheap_undo_reserve(Relation rel, Size len,
							   IHeapUndoReservation *res);
extern UndoRecPtr iheap_undo_write(Relation rel, IHeapUndoReservation *res,
								   IHeapUndoRecHeader *hdr,
								   const char *payload, uint32 payload_len);
extern void iheap_undo_register_wal(IHeapUndoReservation *res, uint8 block_id,
									BlockNumber datablk);
extern void iheap_undo_redo_records(XLogReaderState *record, Page page,
									const char *data, Size len);
extern void iheap_undo_set_lsn(IHeapUndoReservation *res, XLogRecPtr lsn);
extern void iheap_undo_release(IHeapUndoReservation *res);
extern IHeapUndoRec *iheap_undo_fetch(Relation rel, UndoRecPtr urp,
									  bool missing_ok);
extern IHeapUndoRec *iheap_undo_fetch_version_rec(Relation rel,
												  const IHeapHdr *h);
extern UndoRecPtr iheap_version_urec(Relation rel, const IHeapHdr *h);
extern void iheap_undo_forget_relation(RelFileLocator rlocator);
extern void iheap_undo_reset_backend(void);
extern uint32 iheap_undo_take_alloc_count(Relation rel);
extern void iheap_undo_init_page(Page page, uint64 lpage);
extern void iheap_undo_put_record(Page page, uint32 offset, const char *rec,
								  uint32 len);
extern int	iheap_undo_lockers(const IHeapUndoRec *rec, IHeapLocker *lockers);
extern uint32 iheap_undo_payload_core_len(const IHeapUndoRec *rec);
extern uint32 iheap_lockers_size(int nlockers);
extern void iheap_lockers_serialize(char *dst, const IHeapLocker *lockers,
									int nlockers);

/* iheap_visibility.c */
typedef enum IHeapXidStatus
{
	IHX_COMMITTED,
	IHX_ABORTED,
	IHX_IN_PROGRESS,
	IHX_CURRENT,
} IHeapXidStatus;

extern IHeapXidStatus iheap_xid_status(TransactionId xid, Buffer buffer,
									   char *tuple_on_page);
extern bool iheap_xid_aborted(TransactionId xid);
extern bool iheap_fetch_version(Relation rel, Buffer buffer, Page page,
								OffsetNumber off, Snapshot snapshot,
								IHeapVersion *v);
extern bool iheap_is_frozen(Relation rel, const IHeapHdr *h);
extern bool iheap_rebuild_prior(Relation rel, char **tuple, uint32 *len,
								IHeapHdr *h, IHeapUndoRec *rec, char **mem);
extern bool iheap_resolve_version(Relation rel, Buffer buffer, char *pagetup,
								  uint32 pagelen, char *copytup,
								  Snapshot snapshot, IHeapVersion *v);
extern bool iheap_resolve_version_ext(Relation rel, Buffer buffer,
									  char *pagetup, uint32 pagelen,
									  char *copytup, Snapshot snapshot,
									  IHeapVersion *v, bool settled_only,
									  bool check_ssi);
extern void iheap_xid_cache_reset(void);

/* iheap_dml.c */
extern void iheap_tuple_insert(Relation rel, TupleTableSlot *slot,
							   CommandId cid, uint32 options,
							   BulkInsertStateData *bistate);
extern void iheap_tuple_insert_speculative(Relation rel, TupleTableSlot *slot,
										   CommandId cid, uint32 options,
										   BulkInsertStateData *bistate,
										   uint32 specToken);
extern void iheap_tuple_complete_speculative(Relation rel, TupleTableSlot *slot,
											 uint32 specToken, bool succeeded);
extern void iheap_multi_insert(Relation rel, TupleTableSlot **slots, int nslots,
							   CommandId cid, uint32 options,
							   BulkInsertStateData *bistate);
extern TM_Result iheap_tuple_delete(Relation rel, ItemPointer tid,
									CommandId cid, uint32 options,
									Snapshot snapshot, Snapshot crosscheck,
									bool wait, TM_FailureData *tmfd);
extern TM_Result iheap_tuple_update(Relation rel, ItemPointer otid,
									TupleTableSlot *slot, CommandId cid,
									uint32 options, Snapshot snapshot,
									Snapshot crosscheck, bool wait,
									TM_FailureData *tmfd,
									LockTupleMode *lockmode,
									TU_UpdateIndexes *update_indexes);
extern TM_Result iheap_tuple_lock(Relation rel, ItemPointer tid,
								  Snapshot snapshot, TupleTableSlot *slot,
								  CommandId cid, LockTupleMode mode,
								  LockWaitPolicy wait_policy, uint8 flags,
								  TM_FailureData *tmfd);
extern bool iheap_tid_found_dirty(Relation rel, ItemPointer tid);
extern bool iheap_tid_found_dirty_version(Relation rel, ItemPointer tid,
										  UndoRecPtr *version);
extern bool iheap_version_granted(Relation rel, ItemPointer tid,
								  UndoRecPtr version, TransactionId xid,
								  CommandId cid);
extern IHeapHdr iheap_put_lock_record(Relation rel, Buffer buf, OffsetNumber off,
									  const IHeapLocker *lockers, int nlockers,
									  TransactionId xid, CommandId cid);
extern void iheap_relock_after_rollback(Relation rel, Buffer buf,
										OffsetNumber off,
										const IHeapLocker *keep, int nkeep);
extern Buffer iheap_get_buffer_for_tuple(Relation rel, Size len,
										 BulkInsertStateData *bistate,
										 Buffer otherbuf, bool *page_init);
extern Size iheap_page_free_space(Page page);
extern OffsetNumber iheap_page_add_item(Page page, const char *tuple,
										uint32 len, OffsetNumber want);
extern bool iheap_page_place_item(Page page, OffsetNumber off,
								  const char *tuple, uint32 len);

/* iheap_rollback.c */
extern void iheap_xact_register_write(Relation rel, UndoRecPtr urp, Size len);
extern UndoRecPtr iheap_xact_last_urp(Relation rel);
extern bool iheap_rollback_tuple(Relation rel, Buffer buffer, OffsetNumber off);
extern void iheap_xact_forget_relation(RelFileLocator rlocator);
extern int	iheap_rollback_page(Relation rel, Buffer buffer);
extern void iheap_register_xact_callbacks(void);

/* iheap_discard.c */
extern int64 iheap_discard(Relation rel, bool force);
extern TransactionId iheap_oldest_live_xid(Relation rel, TransactionId horizon);

/* iheap_prune.c */
extern bool iheap_page_prune(Relation rel, Buffer buffer, bool force,
							 OffsetNumber *deadoffs, int *ndead);
extern void iheap_page_compact(Relation rel, Page page,
							   const uint32 *reserve);
extern void iheap_compute_reservations(Relation rel, Page page,
									   uint32 *reserve);
extern OffsetNumber iheap_page_choose_offnum(Page page);
extern bool iheap_page_place_item_ext(Page page, OffsetNumber off,
									  const char *tuple, uint32 len,
									  uint32 capacity);
extern uint32 iheap_item_reservation(Relation rel, Page page, OffsetNumber off);

/* iheap_xlog.c */
#define RM_IHEAP_ID		RM_EXPERIMENTAL_ID
extern void iheap_rmgr_register(void);
extern void iheap_redo(XLogReaderState *record);
extern void iheap_desc(StringInfo buf, XLogReaderState *record);
extern const char *iheap_identify(uint8 info);
extern void iheap_mask(char *pagedata, BlockNumber blkno);

/* iheap_scan.c */
extern TableScanDesc iheap_beginscan(Relation rel, Snapshot snapshot,
									 int nkeys, ScanKeyData *key,
									 ParallelTableScanDesc pscan,
									 uint32 flags);
extern void iheap_endscan(TableScanDesc scan);
extern void iheap_rescan(TableScanDesc scan, ScanKeyData *key, bool set_params,
						 bool allow_strat, bool allow_sync,
						 bool allow_pagemode);
extern bool iheap_getnextslot(TableScanDesc scan, ScanDirection direction,
							  TupleTableSlot *slot);
extern void iheap_set_tidrange(TableScanDesc scan, ItemPointer mintid,
							   ItemPointer maxtid);
extern bool iheap_getnextslot_tidrange(TableScanDesc scan,
									   ScanDirection direction,
									   TupleTableSlot *slot);
extern IndexFetchTableData *iheap_index_fetch_begin(Relation rel, uint32 flags);
extern void iheap_index_fetch_reset(IndexFetchTableData *scan);
extern void iheap_index_fetch_end(IndexFetchTableData *scan);
extern bool iheap_index_fetch_tuple(IndexFetchTableData *scan, ItemPointer tid,
									Snapshot snapshot, TupleTableSlot *slot,
									bool *call_again, bool *all_dead);
extern bool iheap_fetch_row_version(Relation rel, ItemPointer tid,
									Snapshot snapshot, TupleTableSlot *slot);
extern bool iheap_tuple_tid_valid(TableScanDesc scan, ItemPointer tid);
extern void iheap_get_latest_tid(TableScanDesc scan, ItemPointer tid);
extern bool iheap_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
										   Snapshot snapshot);
extern bool iheap_scan_bitmap_next_tuple(TableScanDesc scan,
										 TupleTableSlot *slot, bool *recheck,
										 uint64 *lossy_pages,
										 uint64 *exact_pages);
extern bool iheap_scan_sample_next_block(TableScanDesc scan,
										 SampleScanState *scanstate);
extern bool iheap_scan_sample_next_tuple(TableScanDesc scan,
										 SampleScanState *scanstate,
										 TupleTableSlot *slot);
extern bool iheap_scan_analyze_next_block(TableScanDesc scan,
										  ReadStream *stream);
extern bool iheap_scan_analyze_next_tuple(TableScanDesc scan,
										  double *liverows, double *deadrows,
										  TupleTableSlot *slot);
extern int	iheap_collect_page_versions(Relation rel, Buffer buffer,
										Snapshot snapshot, char *pagecopy,
										OffsetNumber *offs, IHeapVersion *vers,
										MemoryContext cxt,
										const OffsetNumber *only, int nonly,
										bool predlock);

/* iheap_vacuum.c */
extern void iheap_vacuum_rel(Relation rel, const VacuumParams *params,
							 BufferAccessStrategy bstrategy);
extern void iheap_page_set_unused(Page page, OffsetNumber *offs, int n);

/* iheap_index.c */
extern double iheap_index_build_range_scan(Relation table_rel,
										   Relation index_rel,
										   IndexInfo *index_info,
										   bool allow_sync, bool anyvisible,
										   bool progress,
										   BlockNumber start_blockno,
										   BlockNumber numblocks,
										   IndexBuildCallback callback,
										   void *callback_state,
										   TableScanDesc scan);
extern void iheap_index_validate_scan(Relation table_rel, Relation index_rel,
									  IndexInfo *index_info, Snapshot snapshot,
									  ValidateIndexState *state);
extern TransactionId iheap_index_delete_tuples(Relation rel,
											   TM_IndexDeleteOp *delstate);

/* iheap_hooks.c */
extern void iheap_hooks_init(void);

/* iheap_dm.c */
extern void iheap_dm_init(void);
extern IHeapDmInfo *iheap_dm_info(Relation rel);
extern void iheap_dm_update_indexes(Relation rel, ItemPointer tid,
									const Bitmapset *modified,
									const Datum *oldvalues,
									const bool *oldisnull,
									const Datum *newvalues,
									const bool *newisnull);
extern int	iheap_dm_matches(Relation rel, const IHeapDmEntry *dm,
							 const char *tuple, bool allow_external);

/* iheap_toast.c */
typedef struct IHeapToastState
{
	ToastTupleContext ttc;
	ToastAttrInfo attr[MaxHeapAttributeNumber];
	bool		active;
} IHeapToastState;

extern void iheap_toast_begin(IHeapToastState *ts, Relation rel,
							  TupleDesc desc, Datum *values, bool *isnull,
							  Datum *oldvalues, bool *oldisnull,
							  uint32 options);
extern void iheap_toast_end(IHeapToastState *ts);
extern void iheap_toast_delete_values(Relation rel, TupleDesc desc,
									  const char *tuple, bool is_speculative);
extern bool iheap_tuple_needs_toast(Relation rel, TupleDesc desc,
									const Datum *values, const bool *isnull,
									bool has_old_external);

/* iheap_rewrite.c */
extern void iheap_relation_copy_for_cluster(Relation OldTable,
											Relation NewTable,
											Relation OldIndex, bool use_sort,
											TransactionId OldestXmin,
											Snapshot snapshot,
											TransactionId *xid_cutoff,
											MultiXactId *multi_cutoff,
											double *num_tuples,
											double *tups_vacuumed,
											double *tups_recently_dead);

#endif							/* IHEAP_H */
