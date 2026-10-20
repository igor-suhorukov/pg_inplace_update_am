/*-------------------------------------------------------------------------
 *
 * dmbtverify_common.h
 *		Shared routines for amcheck verifications (fork of amcheck's
 *		verify_common.h, for dmbtverify.c).
 *
 * Portions Copyright (c) 2026, pg_inplace_update_am authors
 * Portions Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "storage/bufpage.h"
#include "storage/lmgr.h"
#include "storage/lockdefs.h"
#include "utils/relcache.h"
#include "miscadmin.h"

/* Typedef for callback function for dmbt_amcheck_lock_relation_and_check */
typedef void (*IndexDoCheckCallback) (Relation rel,
									  Relation heaprel,
									  void *state,
									  bool readonly);

extern void dmbt_amcheck_lock_relation_and_check(Oid indrelid,
											Oid am_id,
											IndexDoCheckCallback check,
											LOCKMODE lockmode, void *state);
