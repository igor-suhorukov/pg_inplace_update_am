/*-------------------------------------------------------------------------
 *
 * iheap_xlog.h
 *	  WAL record formats of the iheap custom resource manager.
 *
 * Every data-changing operation writes one record that registers the data
 * page(s), the undo page and, when visibility-map bits are cleared, the VM
 * page(s).  The undo page's block data holds the undo records the operation
 * wrote, each in the form iheap_undo.c describes (their page encoding, with
 * what redo can derive left out).
 *
 * Tuple headers travel without their xid and with their undo pointer as an
 * offset on the record's undo page (§21 D42): redo replays the undo page
 * first and takes its logical page number from it.
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#ifndef IHEAP_XLOG_H
#define IHEAP_XLOG_H

#include "access/xlogreader.h"
#include "storage/off.h"

#define XLOG_IHEAP_INSERT		0x00
#define XLOG_IHEAP_MULTI_INSERT	0x10
#define XLOG_IHEAP_DELETE		0x20
#define XLOG_IHEAP_UPDATE		0x30	/* in place */
#define XLOG_IHEAP_MOVE			0x40	/* non-in-place update */
#define XLOG_IHEAP_LOCK			0x50
#define XLOG_IHEAP_ROLLBACK		0x60	/* undo applied to one item */
#define XLOG_IHEAP_PRUNE		0x70
#define XLOG_IHEAP_VISIBLE		0x80
#define XLOG_IHEAP_UNDO_ALLOC	0x90
#define XLOG_IHEAP_DISCARD		0xA0
#define XLOG_IHEAP_META			0xB0
#define XLOG_IHEAP_SPEC_FINISH	0xC0
#define XLOG_IHEAP_UNUSED		0xD0	/* vacuum: LP_DEAD -> LP_UNUSED */

#define XLOG_IHEAP_OPMASK		0xF0

/* flags shared by data-changing records */
#define XLIH_INIT_PAGE			0x01	/* data page (re)initialized */
#define XLIH_VM_CLEARED			0x02	/* VM bits of the data page cleared */
#define XLIH_NEW_INIT_PAGE		0x04	/* MOVE: new page initialized */
#define XLIH_NEW_VM_CLEARED		0x08	/* MOVE: VM bits of new page cleared */
#define XLIH_SAME_PAGE			0x10	/* MOVE: new tuple on the same page */
#define XLIH_RELOCATED			0x20	/* UPDATE: tuple moved within the page */
#define XLIH_LOGICAL			0x40	/* INSERT, MULTI_INSERT, UPDATE, MOVE,
										 * DELETE: main data carries what
										 * logical decoding needs (below) */
#define XLIH_LAST_IN_MULTI		0x80	/* MULTI_INSERT: the last record of a
										 * multi_insert call */
#define XLIH_MOVE_DELTA			0x20	/* MOVE on the same page: the new body
										 * logged as the middle that differs
										 * from the old one (below) */

/*
 * With XLIH_LOGICAL, the record's struct is followed in main data by a flags
 * byte and the parts it names (iheap_logical.c): the layout of the
 * relation's attributes, for the iheap tuple the record carries anyway
 * (whole, and kept with a full-page image: REGBUF_KEEP_DATA), and the old
 * row's replica identity in heap's layout.
 */
#define XLIL_LAYOUT				0x01
#define XLIL_OLD_KEY			0x02	/* the identity columns */
#define XLIL_OLD_TUPLE			0x04	/* the whole row (REPLICA IDENTITY FULL) */

/* block ids */
#define IH_BLK_DATA			0
#define IH_BLK_UNDO			1
#define IH_BLK_VM			2
#define IH_BLK_NEWDATA		3	/* MOVE */
#define IH_BLK_NEWVM		4	/* MOVE */
#define IH_BLK_META			0	/* UNDO_ALLOC, DISCARD, META */
#define IH_BLK_NEWUNDO		1	/* UNDO_ALLOC */

/*
 * The header of a tuple a record writes, as it travels in WAL:
 *
 *		uint16 infomask2, uint16 infomask, uint8 hoff, uint16 urec_off
 *
 * unaligned.  The tuple's header on the page takes the form its infomask
 * gives (IHT_COMPACT, §21 D43).  Its undo pointer is urec_off on the
 * record's undo page, or 0 when urec_off is 0 (a frozen insert).  Its xid
 * is not logged: a compact header has none; otherwise it is the record's
 * xid, or FrozenTransactionId for a frozen insert.
 */
#define SizeOfIHeapWalHdr	7

/*
 * The header an existing item gets (DELETE, LOCK, the old item of a MOVE),
 * as it travels in WAL:
 *
 *		uint16 infomask, uint16 urec_off
 *
 * infomask2 and hoff are those of the item's header on the page, and so is
 * the xid of a LOCK (a lock does not change the version's creator); a
 * DELETE or MOVE sets the record's xid.  A compact header that has to
 * carry an xid becomes a full one: the item turns into a stub (IHT_STUB in
 * the new infomask, its body in the undo record) and hoff grows with it.
 */
#define SizeOfIHeapWalHdrChange	4

/* XLOG_IHEAP_INSERT: block DATA data = header (SizeOfIHeapWalHdr), body */
typedef struct xl_iheap_insert
{
	OffsetNumber offnum;
	uint8		flags;
} xl_iheap_insert;

#define SizeOfIHeapInsert	(offsetof(xl_iheap_insert, flags) + sizeof(uint8))

/*
 * XLOG_IHEAP_MULTI_INSERT: block DATA data = ntuples entries of
 *		{ OffsetNumber offnum; uint16 bodylen; uint16 infomask2;
 *		  uint16 infomask; uint8 hoff; char body[bodylen] }
 * All tuples share the record's xid and its undo record at urec_off (0 for a
 * frozen load: no undo, xid FrozenTransactionId).
 */
typedef struct xl_iheap_multi_insert
{
	uint16		ntuples;
	uint16		urec_off;
	uint8		flags;
} xl_iheap_multi_insert;

#define SizeOfIHeapMultiInsert	(offsetof(xl_iheap_multi_insert, flags) + sizeof(uint8))
#define SizeOfIHeapMultiTuple	(2 + 2 + 2 + 2 + 1)

/* XLOG_IHEAP_DELETE, XLOG_IHEAP_LOCK: block DATA data = header change */
typedef struct xl_iheap_hdrchange
{
	OffsetNumber offnum;
	uint8		flags;
} xl_iheap_hdrchange;

#define SizeOfIHeapHdrChange	(offsetof(xl_iheap_hdrchange, flags) + sizeof(uint8))

/*
 * XLOG_IHEAP_UPDATE: block DATA data =
 *		header (SizeOfIHeapWalHdr) + new body bytes between prefix and suffix
 * The new body is old_body[0:prefix] + mid + old_body[old_bodylen-suffix:].
 * With XLIH_LOGICAL, prefix and suffix are 0: the whole new body.  Without
 * it, the update's undo record leaves its delta header out of WAL: redo
 * takes prefix and suffix from here.
 */
typedef struct xl_iheap_update
{
	OffsetNumber offnum;
	uint16		new_len;		/* tuple length incl. header */
	uint16		lp_off;			/* where the tuple lives after the update */
	uint16		prefix;
	uint16		suffix;
	uint16		capacity;		/* reserved item size when relocated, else 0 */
	uint8		flags;
} xl_iheap_update;

#define SizeOfIHeapUpdate	(offsetof(xl_iheap_update, flags) + sizeof(uint8))

/*
 * XLOG_IHEAP_MOVE: block DATA data = header change of the old item;
 * block NEWDATA (or DATA when XLIH_SAME_PAGE, appended) data = the new
 * tuple: header (SizeOfIHeapWalHdr), body.  With XLIH_MOVE_DELTA (a move on
 * the same page, the old body still there: §21 D40), main data goes on with
 * xl_iheap_move_delta, and only the new body's bytes between prefix and
 * suffix are logged, as heap does for an update on the same page.
 */
typedef struct xl_iheap_move
{
	OffsetNumber old_offnum;
	OffsetNumber new_offnum;
	uint16		new_len;
	uint8		flags;
} xl_iheap_move;

#define SizeOfIHeapMove		(offsetof(xl_iheap_move, flags) + sizeof(uint8))

typedef struct xl_iheap_move_delta
{
	uint16		prefix;
	uint16		suffix;
} xl_iheap_move_delta;

/* XLOG_IHEAP_ROLLBACK */
#define XLIH_RB_KILL		1	/* item becomes LP_DEAD */
#define XLIH_RB_HEADER		2	/* restore header only (data: its image) */
#define XLIH_RB_TUPLE		3	/* restore whole tuple at lp_off */

typedef struct xl_iheap_rollback
{
	OffsetNumber offnum;
	uint16		lp_off;
	uint16		len;
	uint8		action;
} xl_iheap_rollback;

#define SizeOfIHeapRollback	(offsetof(xl_iheap_rollback, action) + sizeof(uint8))

/*
 * XLOG_IHEAP_PRUNE: block DATA data =
 *		OffsetNumber dead[ndead]		items that become LP_DEAD
 *		OffsetNumber stub[nstub]		items shrunk to a header-only stub
 *		{ OffsetNumber off; uint16 len } reserve[nreserve]
 * followed by compaction of the page.  With XLIH_PRUNE_CONFLICT, main data
 * goes on with the newest xid whose deleted or moved rows lost their body
 * (standby queries that may still see them are cancelled first).
 */
typedef struct xl_iheap_prune
{
	uint16		ndead;
	uint16		nstub;
	uint16		nreserve;
	uint8		flags;
} xl_iheap_prune;

#define XLIH_PRUNE_DONE			0x01	/* the page has nothing left to prune */
#define XLIH_PRUNE_CONFLICT		0x02	/* a conflict horizon follows */

#define SizeOfIHeapPrune	(offsetof(xl_iheap_prune, flags) + sizeof(uint8))

/* XLOG_IHEAP_VISIBLE: block DATA = data page, block VM = VM page */
typedef struct xl_iheap_visible
{
	uint8		vmflags;
} xl_iheap_visible;

#define SizeOfIHeapVisible	(sizeof(uint8))

/*
 * XLOG_IHEAP_UNDO_ALLOC: block META data = metapage contents,
 * block NEWUNDO = new undo page (REGBUF_WILL_INIT).
 */
typedef struct xl_iheap_undo_alloc
{
	uint64		lpage;
} xl_iheap_undo_alloc;

#define SizeOfIHeapUndoAlloc	(sizeof(uint64))

/* XLOG_IHEAP_DISCARD: block META data = metapage contents */
typedef struct xl_iheap_discard
{
	TransactionId snapshotConflictHorizon;
	uint8		flags;
} xl_iheap_discard;

#define SizeOfIHeapDiscard	(offsetof(xl_iheap_discard, flags) + sizeof(uint8))

/*
 * XLOG_IHEAP_SPEC_FINISH: no block data; a confirmation clears the item's
 * IHT_SPECULATIVE flag, a super-deletion makes it LP_DEAD.
 */
typedef struct xl_iheap_spec_finish
{
	OffsetNumber offnum;
	uint8		flags;
} xl_iheap_spec_finish;

#define XLIH_SPEC_SUCCEEDED		0x01	/* confirmed (else super-deleted) */
#define XLIH_SPEC_LOGICAL		0x02	/* the insertion was logged for logical
										 * decoding: decode this too */

#define SizeOfIHeapSpecFinish	(offsetof(xl_iheap_spec_finish, flags) + sizeof(uint8))

/* XLOG_IHEAP_UNUSED: block DATA data = OffsetNumber[noffsets] */
typedef struct xl_iheap_unused
{
	uint16		noffsets;
} xl_iheap_unused;

#define SizeOfIHeapUnused	(sizeof(uint16))

/* ------------------------------------------------------------------------
 * Tuple headers in WAL (iheap_xlog.c)
 * ------------------------------------------------------------------------
 */
struct IHeapHdr;

extern void iheap_walhdr_put(char *dst, const struct IHeapHdr *h);
extern uint16 iheap_walhdr_urec_off(const char *src);
extern void iheap_walhdr_get(const char *src, struct IHeapHdr *h,
							 uint64 lpage, TransactionId xid);
extern void iheap_walchange_put(char *dst, const struct IHeapHdr *h);
extern uint16 iheap_walchange_urec_off(const char *src);
extern void iheap_walchange_get(const char *src, const struct IHeapHdr *old,
								struct IHeapHdr *h, uint64 lpage,
								TransactionId xid);

#endif							/* IHEAP_XLOG_H */
