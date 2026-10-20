/*-------------------------------------------------------------------------
 *
 * iheap_handler.c
 *	  Table access method routine, relation-level callbacks and module
 *	  initialization of iheap.
 *
 * The module must be loaded through shared_preload_libraries: its WAL
 * resource managers (iheap's and dmbtree's) have to be registered before
 * recovery starts (DESIGN.md §21 D15).
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/heaptoast.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/pg_am_d.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "optimizer/plancat.h"
#include "storage/bufmgr.h"
#include "storage/bulk_write.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "iheap.h"
#include "dmbtree_api.h"

PG_MODULE_MAGIC_EXT(.name = "pg_inplace_update_am",.version = "0.1");

bool		iheap_enable_inplace = true;
int			iheap_foreground_undo_limit = 1024; /* kB */

static const struct config_enum_entry logical_decoding_options[] = {
	{"error", 0, false},
	{"ignore", 1, false},
	{NULL, 0, false}
};

/* ------------------------------------------------------------------------
 * Slot callbacks
 * ------------------------------------------------------------------------
 */
static const TupleTableSlotOps *
iheap_slot_callbacks(Relation relation)
{
	return &TTSOpsIHeapTuple;
}

/* ------------------------------------------------------------------------
 * Storage management
 * ------------------------------------------------------------------------
 */

/*
 * Write a fresh metapage as block 0 of a fork that has just been created.
 * The relcache entry may still point at the old storage (TRUNCATE), so this
 * works at the smgr level, like btbuildempty().
 */
static void
iheap_write_new_metapage(SMgrRelation srel, ForkNumber forknum, bool use_wal)
{
	BulkWriteState *bulkstate;
	BulkWriteBuffer buf;

	bulkstate = smgr_bulk_start_smgr(srel, forknum, use_wal);
	buf = smgr_bulk_get_buf(bulkstate);
	iheap_init_metapage((Page) buf);
	smgr_bulk_write(bulkstate, IHEAP_METAPAGE_BLKNO, buf, true);
	smgr_bulk_finish(bulkstate);
}

static void
iheap_relation_set_new_filelocator(Relation rel,
								   const RelFileLocator *newrlocator,
								   char persistence,
								   TransactionId *freezeXid,
								   MultiXactId *minmulti)
{
	SMgrRelation srel;

	/*
	 * No tuple of the new storage can carry an older xid.  Undo-based
	 * freezing (a tuple is frozen once its undo is discarded) means that
	 * relfrozenxid only has to bound the xids of not-yet-discarded undo.
	 */
	*freezeXid = RecentXmin;
	*minmulti = GetOldestMultiXactId();

	srel = RelationCreateStorage(*newrlocator, persistence, true);

	/* a new relation: its statistics go if its creation rolls back */
	if (rel->rd_createSubid == GetCurrentSubTransactionId())
		iheap_stat_relation_created(RelationGetRelid(rel));

	/*
	 * A permanent relation's new storage is WAL-logged unless wal_level is
	 * minimal, in which case RelationCreateStorage() arranged a sync at
	 * commit.  Unlogged relations get an init fork holding the metapage, so
	 * that the main fork is valid again after a crash reset.
	 */
	iheap_write_new_metapage(srel, MAIN_FORKNUM,
							 persistence == RELPERSISTENCE_PERMANENT &&
							 XLogIsNeeded());

	if (persistence == RELPERSISTENCE_UNLOGGED)
	{
		Assert(rel->rd_rel->relkind == RELKIND_RELATION ||
			   rel->rd_rel->relkind == RELKIND_MATVIEW ||
			   rel->rd_rel->relkind == RELKIND_TOASTVALUE);
		smgrcreate(srel, INIT_FORKNUM, false);
		log_smgrcreate(newrlocator, INIT_FORKNUM);
		iheap_write_new_metapage(srel, INIT_FORKNUM, true);
	}

	smgrclose(srel);
}

/*
 * Truncate in place.  Only used for storage created by the current
 * subtransaction (or ON COMMIT DELETE ROWS of a temp table), so nothing
 * written there needs to survive an abort.
 */
static void
iheap_relation_nontransactional_truncate(Relation rel)
{
	Buffer		buf;

	RelationTruncate(rel, 0);

	iheap_relcache_invalidate(rel);
	iheap_undo_forget_relation(rel->rd_locator);
	iheap_xact_forget_relation(rel->rd_locator);

	buf = ExtendBufferedRel(BMR_REL(rel), MAIN_FORKNUM, NULL, EB_LOCK_FIRST);
	if (BufferGetBlockNumber(buf) != IHEAP_METAPAGE_BLKNO)
		elog(ERROR, "iheap: unexpected block %u after truncating \"%s\"",
			 BufferGetBlockNumber(buf), RelationGetRelationName(rel));

	START_CRIT_SECTION();
	iheap_init_metapage(BufferGetPage(buf));
	MarkBufferDirty(buf);
	if (RelationNeedsWAL(rel))
		log_newpage_buffer(buf, true);
	END_CRIT_SECTION();

	UnlockReleaseBuffer(buf);
}

/*
 * Copy all forks to new storage (ALTER TABLE ... SET TABLESPACE).  Undo
 * lives in the main fork, so a block-by-block copy keeps it consistent.
 */
static void
iheap_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	SMgrRelation dstrel;

	FlushRelationBuffers(rel);

	dstrel = RelationCreateStorage(*newrlocator, rel->rd_rel->relpersistence,
								   true);

	RelationCopyStorage(RelationGetSmgr(rel), dstrel, MAIN_FORKNUM,
						rel->rd_rel->relpersistence);

	for (ForkNumber forkNum = MAIN_FORKNUM + 1;
		 forkNum <= MAX_FORKNUM; forkNum++)
	{
		if (smgrexists(RelationGetSmgr(rel), forkNum))
		{
			smgrcreate(dstrel, forkNum, false);

			if (RelationIsPermanent(rel) ||
				(rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
				 forkNum == INIT_FORKNUM))
				log_smgrcreate(newrlocator, forkNum);
			RelationCopyStorage(RelationGetSmgr(rel), dstrel, forkNum,
								rel->rd_rel->relpersistence);
		}
	}

	RelationDropStorage(rel);
	smgrclose(dstrel);

	iheap_undo_forget_relation(rel->rd_locator);
}

/* ------------------------------------------------------------------------
 * Planner support
 * ------------------------------------------------------------------------
 */
static bool
iheap_relation_needs_toast_table(Relation rel)
{
	int32		data_length = 0;
	bool		maxlength_unknown = false;
	bool		has_toastable_attrs = false;
	TupleDesc	tupdesc = rel->rd_att;
	int32		tuple_length;

	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		if (att->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
			continue;
		if (att->attlen > 0)
		{
			/* stored unaligned: no padding before fixed-length values */
			data_length += att->attlen;
		}
		else
		{
			int32		maxlen = type_maximum_size(att->atttypid,
												   att->atttypmod);

			data_length = att_align_nominal(data_length, att->attalign);
			if (maxlen < 0)
				maxlength_unknown = true;
			else
				data_length += maxlen;
			if (att->attstorage != TYPSTORAGE_PLAIN)
				has_toastable_attrs = true;
		}
	}
	if (!has_toastable_attrs)
		return false;
	if (maxlength_unknown)
		return true;
	tuple_length = IHEAP_TUPHDR_SIZE + BITMAPLEN(tupdesc->natts) +
		MAXALIGN(data_length);
	return (tuple_length > TOAST_TUPLE_THRESHOLD);
}

static Oid
iheap_relation_toast_am(Relation rel)
{
	return HEAP_TABLE_AM_OID;
}

/*
 * As table_block_relation_estimate_size(), but over data pages: the
 * metapage and undo extents hold no tuples and are not read by scans.
 * pg_class.relpages counts all blocks (ANALYZE sets it so), so the density
 * of the last ANALYZE or VACUUM is taken over its blocks minus the current
 * non-data blocks (undo extents change slowly).
 */
static void
iheap_relation_estimate_size(Relation rel, int32 *attr_widths,
							 BlockNumber *pages, double *tuples,
							 double *allvisfrac)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	BlockNumber nondata = Min(nblocks, 1 + iheap_extent_pages(rel));
	BlockNumber datapages = nblocks - nondata;
	BlockNumber relpages = (BlockNumber) rel->rd_rel->relpages;
	double		reltuples = (double) rel->rd_rel->reltuples;
	BlockNumber relallvisible = (BlockNumber) rel->rd_rel->relallvisible;
	double		density;

	/* heap's rule: a never-vacuumed table is assumed to be at least 10 pages */
	if (datapages < 10 && relpages == 0 && !rel->rd_rel->relhassubclass)
		datapages = 10;

	*pages = datapages;
	if (datapages == 0)
	{
		*tuples = 0;
		*allvisfrac = 0;
		return;
	}

	if (reltuples >= 0 && relpages > 0)
	{
		BlockNumber then_data = relpages > nondata ? relpages - nondata : 1;

		density = reltuples / (double) then_data;
	}
	else
	{
		int32		tuple_width;
		int			fillfactor = RelationGetFillFactor(rel, HEAP_DEFAULT_FILLFACTOR);

		tuple_width = get_rel_data_width(rel, attr_widths);
		tuple_width += IHEAP_TUPHDR_SIZE + sizeof(ItemIdData);
		density = ((double) (BLCKSZ - SizeOfPageHeaderData - IHEAP_SPECIAL_SIZE) *
				   fillfactor / 100) / tuple_width;
		if (density < 1)
			density = 1;
	}
	*tuples = rint(density * (double) datapages);

	if (relallvisible == 0)
		*allvisfrac = 0;
	else if ((double) relallvisible >= datapages)
		*allvisfrac = 1;
	else
		*allvisfrac = (double) relallvisible / datapages;
}

/* ------------------------------------------------------------------------
 * The routine
 * ------------------------------------------------------------------------
 */
static const TableAmRoutine iheap_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = iheap_slot_callbacks,

	.scan_begin = iheap_beginscan,
	.scan_end = iheap_endscan,
	.scan_rescan = iheap_rescan,
	.scan_getnextslot = iheap_getnextslot,

	.scan_set_tidrange = iheap_set_tidrange,
	.scan_getnextslot_tidrange = iheap_getnextslot_tidrange,

	.parallelscan_estimate = table_block_parallelscan_estimate,
	.parallelscan_initialize = table_block_parallelscan_initialize,
	.parallelscan_reinitialize = table_block_parallelscan_reinitialize,

	.index_fetch_begin = iheap_index_fetch_begin,
	.index_fetch_reset = iheap_index_fetch_reset,
	.index_fetch_end = iheap_index_fetch_end,
	.index_fetch_tuple = iheap_index_fetch_tuple,

	.tuple_insert = iheap_tuple_insert,
	.tuple_insert_speculative = iheap_tuple_insert_speculative,
	.tuple_complete_speculative = iheap_tuple_complete_speculative,
	.multi_insert = iheap_multi_insert,
	.tuple_delete = iheap_tuple_delete,
	.tuple_update = iheap_tuple_update,
	.tuple_lock = iheap_tuple_lock,

	.tuple_fetch_row_version = iheap_fetch_row_version,
	.tuple_get_latest_tid = iheap_get_latest_tid,
	.tuple_tid_valid = iheap_tuple_tid_valid,
	.tuple_satisfies_snapshot = iheap_tuple_satisfies_snapshot,
	.index_delete_tuples = iheap_index_delete_tuples,

	.relation_set_new_filelocator = iheap_relation_set_new_filelocator,
	.relation_nontransactional_truncate = iheap_relation_nontransactional_truncate,
	.relation_copy_data = iheap_relation_copy_data,
	.relation_copy_for_cluster = iheap_relation_copy_for_cluster,
	.relation_vacuum = iheap_vacuum_rel,
	.scan_analyze_next_block = iheap_scan_analyze_next_block,
	.scan_analyze_next_tuple = iheap_scan_analyze_next_tuple,
	.index_build_range_scan = iheap_index_build_range_scan,
	.index_validate_scan = iheap_index_validate_scan,

	.relation_size = table_block_relation_size,
	.relation_needs_toast_table = iheap_relation_needs_toast_table,
	.relation_toast_am = iheap_relation_toast_am,
	.relation_fetch_toast_slice = heap_fetch_toast_slice,

	.relation_estimate_size = iheap_relation_estimate_size,

	.scan_bitmap_next_tuple = iheap_scan_bitmap_next_tuple,
	.scan_sample_next_block = iheap_scan_sample_next_block,
	.scan_sample_next_tuple = iheap_scan_sample_next_tuple
};

/*
 * A relation descriptor built from a relfilelocator alone, for work done
 * outside of a normal relation access (the abort callbacks).  It is good
 * for buffer access, smgr calls and WAL logging; it has no tuple
 * descriptor and its lock tag is bogus, so it must not be used for locking.
 */
Relation
iheap_fake_relation_open(RelFileLocator rlocator, char persistence)
{
	Relation	rel = CreateFakeRelcacheEntry(rlocator);

	Assert(persistence != RELPERSISTENCE_TEMP);
	rel->rd_rel->relkind = RELKIND_RELATION;
	rel->rd_rel->relpersistence = persistence;
	rel->rd_tableam = &iheap_methods;
	return rel;
}

void
iheap_fake_relation_close(Relation rel)
{
	iheap_relcache_invalidate(rel);
	FreeFakeRelcacheEntry(rel);
}

bool
iheap_relation_is_iheap(Relation rel)
{
	return rel->rd_tableam == &iheap_methods;
}

PG_FUNCTION_INFO_V1(iheap_tableam_handler);

Datum
iheap_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&iheap_methods);
}

/* ------------------------------------------------------------------------
 * Module initialization
 * ------------------------------------------------------------------------
 */
void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_inplace_update_am must be loaded via \"shared_preload_libraries\""),
				 errdetail("The iheap WAL resource manager has to be registered before recovery starts.")));

	DefineCustomBoolVariable("iheap.enable_inplace",
							 "Allows updates that keep a row at its TID.",
							 "When off, every update moves the row and inserts new index entries.",
							 &iheap_enable_inplace,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("iheap.foreground_undo_limit",
							"Largest amount of undo an aborting transaction applies itself.",
							"Aborted changes beyond this size are rolled back lazily by later accesses and VACUUM.",
							&iheap_foreground_undo_limit,
							1024,
							0, MAX_KILOBYTES,
							PGC_USERSET,
							GUC_UNIT_KB,
							NULL, NULL, NULL);

	DefineCustomEnumVariable("iheap.logical_decoding",
							 "What logical decoding does with changes to iheap tables.",
							 "\"error\" stops decoding; \"ignore\" skips the changes.",
							 &iheap_logical_decoding,
							 0,
							 logical_decoding_options,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);

	iheap_hooks_init();

	MarkGUCPrefixReserved("iheap");

	iheap_stats_init();

	iheap_rmgr_register();
	dmbtree_rmgr_register();
	iheap_register_xact_callbacks();
	iheap_dm_init();
}
