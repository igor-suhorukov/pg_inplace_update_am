#!/usr/bin/env bash
#
# Mechanical part of the dmbtree fork (DESIGN.md §21 D30): copy PostgreSQL's
# nbtree sources, and amcheck's B-tree verification (§21 D34), and rename what
# would clash with the server's own nbtree and with amcheck.
#
#   scripts/dmbtree_fork.sh <postgres-source-root> <output-dir>
#
# Only file names and identifiers change here; every change in behaviour
# (the delete-mark bit, the iheap glue, the resource manager) is made by
# hand in src/dmbtree.  Regenerating from the base commit named in
# src/dmbtree/README and diffing against src/dmbtree therefore shows exactly
# the hand-made part of the fork, which is what has to be carried over when
# nbtree changes in a minor release:
#
#   scripts/dmbtree_fork.sh /usr/src/postgres /tmp/base
#   diff -ru /tmp/base src/dmbtree
#
# Not forked: nbtcompare.c (the opclass support functions are core's own
# pg_proc entries) and the functions that only depend on relation identity
# or on reloption/planner tables (btoptions, btproperty, btbuildphasename,
# bttranslate*, and the vacuum cycle-id bookkeeping _bt_*vacuum*, which lives
# in core's shared memory and is keyed by relation): the fork calls core's.
#
set -euo pipefail

src=${1:?postgres source root}
out=${2:?output directory}
nbt=$src/src/backend/access/nbtree

mkdir -p "$out"

copy() {
	sed -E \
		-e 's/\b_bt_/_dmbt_/g' \
		-e 's/\b_dmbt_(vacuum_cycleid|start_vacuum|end_vacuum|end_vacuum_callback)\b/_bt_\1/g' \
		-e 's/\bbt(beginscan|build|buildempty|bulkdelete|canreturn|endscan|estimateparallelscan|getbitmap|gettreeheight|gettuple|handler|initparallelscan|insert|markpos|parallelrescan|rescan|restrpos|vacuumcleanup|vacuumpage|vacuumscan|reevacuumposting|validate|adjustmembers)\b/dmbt\1/g' \
		-e 's/\bbtree_(redo|mask|desc|identify|xlog_[a-z_]+)\b/dmbtree_\1/g' \
		-e 's/\bRM_BTREE_ID\b/RM_DMBTREE_ID/g' \
		-e 's|"access/nbtree\.h"|"dmbtree.h"|' \
		-e 's|"access/nbtxlog\.h"|"dmbtxlog.h"|' \
		-e 's/\bNBTREE_H\b/DMBTREE_H/g' \
		-e 's/\bNBTXLOG_H\b/DMBTXLOG_H/g' \
		-e 's/\bbt_index_check\b/dmbtree_check/g' \
		-e 's/\bbt_index_parent_check\b/dmbtree_parent_check/g' \
		-e 's/\bamcheck_lock_relation_and_check\b/dmbt_amcheck_lock_relation_and_check/g' \
		-e 's|"verify_common\.h"|"dmbtverify_common.h"|' \
		"$1" >"$out/$2"
}

for f in nbtdedup nbtinsert nbtpage nbtpreprocesskeys nbtreadpage nbtree \
	nbtsearch nbtsort nbtsplitloc nbtutils nbtvalidate nbtxlog; do
	copy "$nbt/$f.c" "dm${f#n}.c"
done
copy "$src/src/backend/access/rmgrdesc/nbtdesc.c" dmbtdesc.c
copy "$src/src/include/access/nbtree.h" dmbtree.h
copy "$src/src/include/access/nbtxlog.h" dmbtxlog.h
copy "$src/contrib/amcheck/verify_nbtree.c" dmbtverify.c
copy "$src/contrib/amcheck/verify_common.c" dmbtverify_common.c
copy "$src/contrib/amcheck/verify_common.h" dmbtverify_common.h
