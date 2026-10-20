/*-------------------------------------------------------------------------
 *
 * iheap_visibility.c
 *	  Tuple visibility: transaction status checks and version-chain walks.
 *
 * A tuple's header names the transaction that created its current version
 * (t_xid) and the undo record describing the previous version (t_urec).
 * Deciding what a snapshot sees means checking t_xid and, when the current
 * version is not visible, rebuilding the previous version from undo and
 * checking again (DESIGN.md §6.3).
 *
 * A version is "frozen" -- visible to everybody, no transaction lookups --
 * when its undo pointer is below the relation's discard pointer: discard
 * only passes records whose writers are committed and visible to all, after
 * applying any aborted ones (§21 D2, D8).
 *
 * Hint bits record a transaction's fate in the tuple header, but only on
 * pages that are already dirty, so that reading never creates a new dirty
 * page (and, with checksums, a full-page image) by itself (§21 D7).
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/subtrans.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lwlock.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "iheap.h"

/* ------------------------------------------------------------------------
 * Backend-local cache of final transaction states
 * ------------------------------------------------------------------------
 */
#define IH_XID_CACHE_SIZE 1024

typedef struct XidCacheEnt
{
	TransactionId xid;
	uint8		status;			/* IHX_COMMITTED or IHX_ABORTED */
} XidCacheEnt;

static XidCacheEnt xid_cache[IH_XID_CACHE_SIZE];

static inline bool
xid_cache_get(TransactionId xid, IHeapXidStatus *status)
{
	XidCacheEnt *e = &xid_cache[xid & (IH_XID_CACHE_SIZE - 1)];

	if (e->xid == xid && TransactionIdIsValid(xid))
	{
		*status = (IHeapXidStatus) e->status;
		return true;
	}
	return false;
}

static inline void
xid_cache_put(TransactionId xid, IHeapXidStatus status)
{
	XidCacheEnt *e = &xid_cache[xid & (IH_XID_CACHE_SIZE - 1)];

	e->xid = xid;
	e->status = (uint8) status;
}

void
iheap_xid_cache_reset(void)
{
	memset(xid_cache, 0, sizeof(xid_cache));
}

/*
 * Did xid commit?  The caller knows it is not running.
 *
 * Xids older than the CLOG horizon can only be referenced through a stale
 * discard pointer; their undo was discarded, which verified them as
 * committed (aborted ones were applied first, so no tuple refers to them).
 * The truncation lock makes the check and the lookup atomic with respect
 * to CLOG truncation, as txid_status() does.
 */
static bool
ih_clog_committed(TransactionId xid)
{
	bool		committed;

	LWLockAcquire(XactTruncationLock, LW_SHARED);
	if (TransactionIdPrecedes(xid, TransamVariables->oldestClogXid))
		committed = true;
	else
		committed = TransactionIdDidCommit(xid);
	LWLockRelease(XactTruncationLock);

	return committed;
}

/*
 * Set a hint bit in the on-page tuple "tup" if the page is already dirty.
 * The caller holds at least a share lock on the buffer.
 */
static void
ih_set_hint(Buffer buffer, char *tup, TransactionId xid, uint16 flag)
{
	uint16		im;

	if (!BufferIsValid(buffer) || tup == NULL)
		return;

	if (flag == IHT_XID_COMMITTED && BufferIsPermanent(buffer))
	{
		XLogRecPtr	commitLSN = TransactionIdGetCommitLSN(xid);

		/* asynchronous commit not yet flushed: don't hint (as heap) */
		if (XLogNeedsFlush(commitLSN) && BufferGetLSNAtomic(buffer) < commitLSN)
			return;
	}

	if (!BufferBeginSetHintBits(buffer))
		return;
	if (BufferIsDirty(buffer))
	{
		im = ih_hdr_infomask(tup);
		if ((im & IHT_XID_HINTS) == 0)
			ih_hdr_set_infomask(tup, im | flag);
	}
	BufferFinishSetHintBits(buffer, false, true);
}

/*
 * Is a locker's xid still running?  As TransactionIdIsInProgress(), except
 * for the aborting backend's own top-level xid during its eager rollback
 * (see iheap_xid_status()): its locks go with its changes.
 */
bool
iheap_xid_running(TransactionId xid)
{
	if (!IsTransactionState() && TransactionIdIsCurrentTransactionId(xid) &&
		TransactionIdDidAbort(xid))
		return false;
	return TransactionIdIsInProgress(xid);
}

/*
 * Status of xid, using hint bits of "tup" (the on-page tuple whose t_xid
 * is xid, or NULL) and setting them when possible.
 */
IHeapXidStatus
iheap_xid_status(TransactionId xid, Buffer buffer, char *tup)
{
	IHeapXidStatus status;
	bool		committed;

	if (!TransactionIdIsNormal(xid))
		return IHX_COMMITTED;

	if (tup != NULL)
	{
		uint16		im = ih_hdr_infomask(tup);

		if (im & IHT_XID_COMMITTED)
			return IHX_COMMITTED;
		if (im & IHT_XID_ABORTED)
			return IHX_ABORTED;
	}

	if (TransactionIdIsCurrentTransactionId(xid))
	{
		/*
		 * The aborting backend's own change, which its eager rollback
		 * (iheap_rollback.c) applies: the top-level xid stays "current"
		 * until CleanupTransaction(), while clog has it aborted already.
		 */
		if (!IsTransactionState() && TransactionIdDidAbort(xid))
			return IHX_ABORTED;
		return IHX_CURRENT;
	}

	if (xid_cache_get(xid, &status))
		return status;

	if (TransactionIdIsInProgress(xid))
		return IHX_IN_PROGRESS;

	committed = ih_clog_committed(xid);
	status = committed ? IHX_COMMITTED : IHX_ABORTED;
	xid_cache_put(xid, status);
	ih_set_hint(buffer, tup, xid, committed ? IHT_XID_COMMITTED : IHT_XID_ABORTED);

	return status;
}

/* aborted, or crashed (neither running nor committed) */
bool
iheap_xid_aborted(TransactionId xid)
{
	return iheap_xid_status(xid, InvalidBuffer, NULL) == IHX_ABORTED;
}

bool
iheap_is_frozen(Relation rel, const IHeapHdr *h)
{
	return h->urec < iheap_discard_ptr(rel);
}

/* MVCC classification of a transaction's change */
typedef enum
{
	IHM_VISIBLE,
	IHM_INVISIBLE,				/* in progress, or committed after snapshot */
	IHM_ABORTED,
	IHM_CURRENT,
} IHeapMvcc;

static IHeapMvcc
ih_mvcc_xid(TransactionId xid, Snapshot snapshot, Buffer buffer, char *tup)
{
	IHeapXidStatus status;
	bool		committed;

	if (!TransactionIdIsNormal(xid))
		return IHM_VISIBLE;

	if (tup != NULL)
	{
		uint16		im = ih_hdr_infomask(tup);

		if (im & IHT_XID_COMMITTED)
			return XidInMVCCSnapshot(xid, snapshot) ? IHM_INVISIBLE : IHM_VISIBLE;
		if (im & IHT_XID_ABORTED)
			return IHM_ABORTED;
	}

	if (TransactionIdIsCurrentTransactionId(xid))
		return IHM_CURRENT;

	if (XidInMVCCSnapshot(xid, snapshot))
		return IHM_INVISIBLE;

	if (xid_cache_get(xid, &status))
		return status == IHX_COMMITTED ? IHM_VISIBLE : IHM_ABORTED;

	/* not in the snapshot, so it finished before the snapshot was taken */
	committed = ih_clog_committed(xid);
	xid_cache_put(xid, committed ? IHX_COMMITTED : IHX_ABORTED);
	ih_set_hint(buffer, tup, xid, committed ? IHT_XID_COMMITTED : IHT_XID_ABORTED);

	return committed ? IHM_VISIBLE : IHM_ABORTED;
}

/*
 * A serializable reader passed over a version of xid that its snapshot does
 * not see: an rw-conflict out to xid, as HeapCheckForSerializableConflictOut()
 * records for every tuple of a chain it looks at.
 */
static void
ih_ssi_conflict_out(Relation rel, TransactionId xid, Snapshot snapshot)
{
	if (!TransactionIdIsNormal(xid) || TransactionIdIsCurrentTransactionId(xid))
		return;
	if (TransactionIdPrecedes(xid, TransactionXmin))
		return;
	xid = SubTransGetTopmostTransaction(xid);
	if (TransactionIdPrecedes(xid, TransactionXmin))
		return;
	CheckForSerializableConflictOut(rel, xid, snapshot);
}

/* ------------------------------------------------------------------------
 * Rebuilding previous versions
 * ------------------------------------------------------------------------
 */

/* allocate an aligned buffer and assemble header + body pieces */
static char *
ih_assemble(const char *hdr, const char *p1, uint32 l1, const char *p2,
			uint32 l2, const char *p3, uint32 l3, uint32 *len, char **memp)
{
	uint32		total = IHEAP_TUPHDR_SIZE + l1 + l2 + l3;
	char	   *mem = palloc(total + MAXIMUM_ALIGNOF);
	char	   *t = mem;
	uint8		hoff = ((const uint8 *) hdr)[4];

	if (ih_tuple_needs_align(hdr))
	{
		uintptr_t	p = TYPEALIGN(MAXIMUM_ALIGNOF, (uintptr_t) mem + hoff);

		t = (char *) (p - hoff);
	}
	memcpy(t, hdr, IHEAP_TUPHDR_SIZE);
	if (l1)
		memcpy(t + IHEAP_TUPHDR_SIZE, p1, l1);
	if (l2)
		memcpy(t + IHEAP_TUPHDR_SIZE + l1, p2, l2);
	if (l3)
		memcpy(t + IHEAP_TUPHDR_SIZE + l1 + l2, p3, l3);
	*len = total;
	*memp = mem;
	return t;
}

/*
 * Given the current version (*tuple, *len) and the undo record that
 * produced it, rebuild the previous version.  On success *tuple and *len point
 * to a new palloc'd tuple and *h holds its header.  Returns false for
 * records without a previous version (inserts).
 */
bool
iheap_rebuild_prior(Relation rel, char **tuple, uint32 *len, IHeapHdr *h,
					IHeapUndoRec *rec, char **mem)
{
	const char *prevhdr = rec->hdr.ur_prev_hdr;
	uint32		core = iheap_undo_payload_core_len(rec);
	char	   *cur = *tuple;
	uint32		curbody = *len - IHEAP_TUPHDR_SIZE;

	switch (rec->hdr.ur_type)
	{
		case IHU_INSERT:
		case IHU_MULTI_INSERT:
			return false;

		case IHU_DELETE:
			*tuple = ih_assemble(prevhdr, rec->payload, core, NULL, 0, NULL, 0,
								 len, mem);
			break;

		case IHU_MOVE:
			if (core < sizeof(ItemPointerData))
				elog(ERROR, "iheap: corrupt MOVE undo record " UINT64_FORMAT,
					 rec->urp);
			*tuple = ih_assemble(prevhdr,
								 rec->payload + sizeof(ItemPointerData),
								 core - sizeof(ItemPointerData),
								 NULL, 0, NULL, 0, len, mem);
			break;

		case IHU_UPDATE:
			{
				IHeapUpdateDelta d;
				const char *mid;

				if (core < sizeof(IHeapUpdateDelta))
					elog(ERROR, "iheap: corrupt UPDATE undo record " UINT64_FORMAT,
						 rec->urp);
				memcpy(&d, rec->payload, sizeof(d));
				mid = rec->payload + sizeof(d);
				if ((uint32) d.prefix + d.suffix > curbody ||
					sizeof(d) + d.mid_len > core ||
					(uint32) d.prefix + d.suffix + d.mid_len + IHEAP_TUPHDR_SIZE != d.old_len)
					elog(ERROR, "iheap: UPDATE undo record " UINT64_FORMAT " does not match the tuple",
						 rec->urp);
				*tuple = ih_assemble(prevhdr,
									 cur + IHEAP_TUPHDR_SIZE, d.prefix,
									 mid, d.mid_len,
									 cur + IHEAP_TUPHDR_SIZE + curbody - d.suffix,
									 d.suffix, len, mem);
				break;
			}

		case IHU_LOCK:
			/* content unchanged; only the header goes back */
			*tuple = ih_assemble(prevhdr, cur + IHEAP_TUPHDR_SIZE, curbody,
								 NULL, 0, NULL, 0, len, mem);
			break;

		default:
			elog(ERROR, "iheap: unknown undo record type %u at " UINT64_FORMAT,
				 rec->hdr.ur_type, rec->urp);
	}

	ih_hdr_read(*tuple, h);
	return true;
}

/* ------------------------------------------------------------------------
 * Resolving the version a snapshot sees
 * ------------------------------------------------------------------------
 */

/*
 * Resolve the version of an item visible to "snapshot".
 *
 * pagetup/pagelen: the tuple in the locked buffer (hints may be set on it);
 * copytup: the same tuple in a private page copy, or NULL.  When the
 * current version is visible, v->tuple points to copytup (or pagetup when
 * there is no copy; the caller copies it before releasing the lock).
 * Versions rebuilt from undo are palloc'd in the current memory context.
 */
bool
iheap_resolve_version(Relation rel, Buffer buffer, char *pagetup,
					  uint32 pagelen, char *copytup, Snapshot snapshot,
					  IHeapVersion *v)
{
	return iheap_resolve_version_ext(rel, buffer, pagetup, pagelen, copytup,
									 snapshot, v, false, false);
}

/*
 * As iheap_resolve_version().  With settled_only, SnapshotAny also passes
 * over versions of transactions that are still running (other than our
 * own): the newest version that is committed or ours.  That is what a
 * heap TID identifies when the executor fetches "the old tuple" of an
 * UPDATE with SnapshotAny; the version of a running transaction may still
 * be rolled back, and must not lend its column values to our update.
 */
bool
iheap_resolve_version_ext(Relation rel, Buffer buffer, char *pagetup,
						  uint32 pagelen, char *copytup, Snapshot snapshot,
						  IHeapVersion *v, bool settled_only, bool check_ssi)
{
	IHeapHdr	h;
	char	   *cur = copytup ? copytup : pagetup;
	uint32		curlen = pagelen;
	char	   *hinttup = pagetup;
	char	   *curmem = NULL;
	bool		from_undo = false;
	UndoRecPtr	discard = iheap_discard_ptr(rel);
	int			steps = 0;
	int			rebuilt = 0;	/* older versions rebuilt from undo */
	CommandId	cmin = InvalidCommandId;

	ih_hdr_read(pagetup, &h);

	v->xmax = InvalidTransactionId;
	v->cmin = InvalidCommandId;
	v->from_undo = false;

	if (snapshot->snapshot_type == SNAPSHOT_DIRTY)
	{
		snapshot->xmin = snapshot->xmax = InvalidTransactionId;
		snapshot->speculativeToken = 0;
	}

	for (;;)
	{
		IHeapUndoRec *rec = NULL;
		bool		deleted = (h.infomask & (IHT_DELETED | IHT_MOVED)) != 0;
		bool		visible = false;
		bool		stepback PG_USED_FOR_ASSERTS_ONLY = false;

		CHECK_FOR_INTERRUPTS();
		if (++steps > 1000000)
			elog(ERROR, "iheap: undo chain too long in relation \"%s\"",
				 RelationGetRelationName(rel));

		/* frozen: committed and visible to everybody */
		if (h.urec < discard)
		{
			if (deleted)
				goto invisible;
			goto found;
		}

		/* a LOCK record in front of the version is transparent */
		if (h.infomask & IHT_LOCKED)
		{
			rec = iheap_undo_fetch(rel, h.urec, true);
			if (rec == NULL)
			{
				/* discarded under us: committed, visible to all */
				if (deleted)
					goto invisible;
				goto found;
			}
			if (rec->hdr.ur_type == IHU_LOCK)
			{
				IHeapHdr	ph;

				ih_hdr_read(rec->hdr.ur_prev_hdr, &ph);
				h.infomask = ph.infomask | (h.infomask & (IHT_XID_HINTS | IHT_STUB));
				h.urec = ph.urec;
				if (ph.xid != h.xid)
				{
					/* should not happen: locks do not change the creator */
					h.xid = ph.xid;
					hinttup = NULL;
				}
				pfree(rec);
				continue;
			}
		}

		switch (snapshot->snapshot_type)
		{
			case SNAPSHOT_MVCC:
				{
					IHeapMvcc	m = ih_mvcc_xid(h.xid, snapshot, buffer, hinttup);

					if (m == IHM_CURRENT)
					{
						if (rec == NULL)
							rec = iheap_undo_fetch(rel, h.urec, false);
						if (rec->hdr.ur_cid < snapshot->curcid)
						{
							if (deleted)
								goto invisible;
							cmin = rec->hdr.ur_cid;
							visible = true;
						}
						else
							stepback = true;
					}
					else if (m == IHM_VISIBLE)
					{
						if (deleted)
							goto invisible;
						visible = true;
					}
					else
					{
						/* passed over a change our snapshot does not see */
						if (m == IHM_INVISIBLE && check_ssi)
							ih_ssi_conflict_out(rel, h.xid, snapshot);
						stepback = true;
					}
					break;
				}

			case SNAPSHOT_SELF:
				{
					IHeapXidStatus st = iheap_xid_status(h.xid, buffer, hinttup);

					if (st == IHX_CURRENT || st == IHX_COMMITTED)
					{
						if (deleted)
							goto invisible;
						visible = true;
					}
					else
						stepback = true;
					break;
				}

			case SNAPSHOT_ANY:
			case SNAPSHOT_TOAST:
				{
					IHeapXidStatus st = iheap_xid_status(h.xid, buffer, hinttup);

					if (st == IHX_ABORTED ||
						(settled_only && st == IHX_IN_PROGRESS))
						stepback = true;
					else if (deleted && (h.infomask & IHT_STUB))
					{
						/* body reclaimed: take the content from undo */
						if (rec == NULL)
							rec = iheap_undo_fetch(rel, h.urec, true);
						if (rec == NULL)
							goto invisible;
						stepback = true;
					}
					else
						visible = true;
					break;
				}

			case SNAPSHOT_DIRTY:
				{
					IHeapXidStatus st = iheap_xid_status(h.xid, buffer, hinttup);

					if (st == IHX_ABORTED)
						stepback = true;
					else if (deleted)
					{
						if (st == IHX_IN_PROGRESS)
						{
							snapshot->xmax = h.xid;
							v->xmax = h.xid;
							visible = true;
						}
						else
							goto invisible;
					}
					else
					{
						if (st == IHX_IN_PROGRESS)
						{
							snapshot->xmin = h.xid;
							if (h.infomask & IHT_SPECULATIVE)
							{
								if (rec == NULL)
									rec = iheap_undo_fetch(rel, h.urec, false);
								if (rec->hdr.ur_type == IHU_INSERT &&
									(rec->hdr.ur_flags & IHUF_SPEC) &&
									rec->payload_len >= sizeof(uint32))
								{
									uint32		token;

									memcpy(&token, rec->payload, sizeof(uint32));
									snapshot->speculativeToken = token;
								}
							}
						}
						visible = true;
					}
					break;
				}

			case SNAPSHOT_NON_VACUUMABLE:
				{
					IHeapXidStatus st = iheap_xid_status(h.xid, buffer, hinttup);

					if (st == IHX_ABORTED)
						stepback = true;
					else if (deleted && st == IHX_COMMITTED &&
							 GlobalVisTestIsRemovableXid(snapshot->vistest, h.xid, true))
						goto invisible;
					else
						visible = true;
					break;
				}

			case SNAPSHOT_HISTORIC_MVCC:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("the iheap access method does not support historic MVCC snapshots")));
				break;
		}

		if (visible)
		{
			if (rec)
				pfree(rec);
			goto found;
		}

		Assert(stepback);

		if (rec == NULL)
			rec = iheap_undo_fetch(rel, h.urec, true);
		if (rec == NULL)
		{
			/*
			 * The record was discarded concurrently (stale discard pointer
			 * in our cache): its writer is committed and visible to all.
			 */
			if (deleted)
				goto invisible;
			goto found;
		}

		{
			char	   *newmem;

			if (!iheap_rebuild_prior(rel, &cur, &curlen, &h, rec, &newmem))
			{
				pfree(rec);
				goto invisible;
			}
			if (curmem)
				pfree(curmem);
			curmem = newmem;
		}
		pfree(rec);
		from_undo = true;
		rebuilt++;
		hinttup = NULL;
		cmin = InvalidCommandId;
	}

found:
	if (rebuilt > 0)
		iheap_stat_count_walk(rel, rebuilt);
	v->tuple = cur;
	v->len = curlen;
	v->mem = curmem;
	v->from_undo = from_undo;
	v->xmin = h.xid;
	v->cmin = cmin;
	if (h.infomask & (IHT_DELETED | IHT_MOVED))
	{
		/*
		 * A deleted version (SnapshotAny, SnapshotDirty): h.xid deleted it,
		 * as heap's xmax says; the content was made by the one before.
		 */
		IHeapUndoRec *rec = iheap_undo_fetch(rel, h.urec, true);

		v->xmax = h.xid;
		v->xmin = FrozenTransactionId;
		if (rec != NULL)
		{
			IHeapHdr	ph;

			ih_hdr_read(rec->hdr.ur_prev_hdr, &ph);
			if (ph.urec >= discard)
				v->xmin = ph.xid;
			pfree(rec);
		}
	}
	return true;

invisible:
	if (rebuilt > 0)
		iheap_stat_count_walk(rel, rebuilt);
	if (curmem)
		pfree(curmem);
	v->mem = NULL;
	return false;
}

bool
iheap_fetch_version(Relation rel, Buffer buffer, Page page, OffsetNumber off,
					Snapshot snapshot, IHeapVersion *v)
{
	ItemId		lp;

	if (off < FirstOffsetNumber || off > PageGetMaxOffsetNumber(page))
		return false;
	lp = PageGetItemId(page, off);
	if (!ItemIdIsNormal(lp))
		return false;

	return iheap_resolve_version(rel, buffer, (char *) page + ItemIdGetOffset(lp),
								 ItemIdGetLength(lp), NULL, snapshot, v);
}
