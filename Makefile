# pg_inplace_update_am: the iheap table access method (PGXS build)
#
# Build and test only inside the Docker images (DESIGN.md §20):
#   scripts/docker.sh build
#   scripts/docker.sh test [regress|isolation|tap|core|all]

MODULE_big = pg_inplace_update_am
OBJS = \
	$(WIN32RES) \
	src/iheap_check.o \
	src/iheap_discard.o \
	src/iheap_dm.o \
	src/iheap_dml.o \
	src/iheap_funcs.o \
	src/iheap_handler.o \
	src/iheap_hooks.o \
	src/iheap_index.o \
	src/iheap_meta.o \
	src/iheap_prune.o \
	src/iheap_rewrite.o \
	src/iheap_rollback.o \
	src/iheap_scan.o \
	src/iheap_slot.o \
	src/iheap_stats.o \
	src/iheap_toast.o \
	src/iheap_tuple.o \
	src/iheap_undo.o \
	src/iheap_vacuum.o \
	src/iheap_visibility.o \
	src/iheap_xlog.o \
	src/dmbtree/dmbtapi.o \
	src/dmbtree/dmbtdedup.o \
	src/dmbtree/dmbtdesc.o \
	src/dmbtree/dmbtinsert.o \
	src/dmbtree/dmbtpage.o \
	src/dmbtree/dmbtpreprocesskeys.o \
	src/dmbtree/dmbtreadpage.o \
	src/dmbtree/dmbtree.o \
	src/dmbtree/dmbtsearch.o \
	src/dmbtree/dmbtsort.o \
	src/dmbtree/dmbtsplitloc.o \
	src/dmbtree/dmbtutils.o \
	src/dmbtree/dmbtvalidate.o \
	src/dmbtree/dmbtverify.o \
	src/dmbtree/dmbtverify_common.o \
	src/dmbtree/dmbtxlog.o

EXTENSION = pg_inplace_update_am
DATA = sql/pg_inplace_update_am--0.1.sql
PGFILEDESC = "pg_inplace_update_am - table and index access methods with in-place updates and undo"

PG_CPPFLAGS = -I$(srcdir)/src -I$(srcdir)/src/dmbtree

# Test suites live under test/; test/run.sh runs them against temporary
# instances.  "make installcheck" also works against a server that has
# shared_preload_libraries = 'pg_inplace_update_am'.
REGRESS = $(shell cat $(srcdir)/test/regress_schedule 2>/dev/null)
REGRESS_OPTS = --inputdir=$(srcdir)/test --load-extension=pg_inplace_update_am
ISOLATION = $(shell cat $(srcdir)/test/isolation_schedule 2>/dev/null)
ISOLATION_OPTS = --inputdir=$(srcdir)/test --load-extension=pg_inplace_update_am
TAP_TESTS = 1
PROVE_TESTS = test/t/*.pl

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
