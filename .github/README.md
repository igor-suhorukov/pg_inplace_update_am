# pg_inplace_update_am

A PostgreSQL 19 extension with two access methods:

- **iheap**: a table access method that updates rows in place and keeps
  their previous versions in undo records. Rollback, MVCC visibility for old
  snapshots, VACUUM, WAL and crash recovery, physical and logical
  replication, TOAST, row locks and SSI all work with it.
- **dmbtree**: a fork of PostgreSQL's nbtree with delete-marking. It lets an
  iheap `UPDATE` keep the row in place even when an indexed column changes.

Everything is built as an extension on stock PostgreSQL 19, with no patches
to the core server.

The extension is at version 0.1 and in early development.

## Quick start

The module registers WAL resource managers, so it has to be preloaded:

```
# postgresql.conf
shared_preload_libraries = 'pg_inplace_update_am'
```

```sql
CREATE EXTENSION pg_inplace_update_am;

CREATE TABLE accounts (id int PRIMARY KEY, branch int, balance numeric)
    USING iheap;
CREATE INDEX ON accounts (branch);     -- a dmbtree index (iheap.default_index_am)

UPDATE accounts SET balance = balance + 1 WHERE id = 1;   -- in place
UPDATE accounts SET branch = 2 WHERE id = 1;              -- in place too
```

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `iheap.enable_inplace` | `on` | Allow updates that keep a row at its TID. When off, every update moves the row. |
| `iheap.default_index_am` | `dmbtree` | Access method of `CREATE INDEX` without `USING` on iheap tables. With `btree`, an update of an indexed column moves the row. |
| `iheap.unique_index_am` | `btree` | Access method of unique indexes and of `PRIMARY KEY` / `UNIQUE` constraints. `btree` keeps core's fast path for foreign key checks. |
| `iheap.foreground_undo_limit` | `1024` (kB) | Largest amount of undo an aborting transaction applies itself. Larger aborts are rolled back lazily by later accesses and VACUUM. |
| `iheap.compact_headers` | `auto` | Where rows written frozen (by table rewrites and frozen loads) get 12-byte tuple headers: `off`, `auto`, `always`. |
| `iheap.logical_decoding` | `on` | What logical decoding does with changes to iheap tables: `on` decodes them, `ignore` skips them. |

## SQL functions

- Inspection: `iheap_metapage_info`, `iheap_page_items`,
  `iheap_undo_page_records`, `iheap_relation_stats`, `dmbtree_index_stats`
- Verification: `iheap_check` (tables and their indexes), and
  `dmbtree_check` / `dmbtree_parent_check` (ports of amcheck's B-tree checks)
- Statistics: `iheap_stat`, `iheap_stat_reset`, the `iheap_stat_tables` view
- Maintenance: `iheap_discard` (discard undo no snapshot needs any more),
  `dmbtree_copy_btree_opclasses` (copy btree operator classes created after
  the extension)

## Building and testing

Builds and tests run only in Docker containers; nothing is compiled on the
host. The image builds PostgreSQL 19 from a pinned `REL_19_STABLE` commit.

```
scripts/docker.sh image            # debug image (assertions, injection points)
scripts/docker.sh build            # compile with -Werror
scripts/docker.sh test             # all suites
scripts/docker.sh test regress isolation tap model
```

The suites:

- regression and isolation tests
- TAP tests: crash recovery, standby, logical replication, PITR,
  pg_upgrade, pg_rewind, random workloads compared against heap, and crashes
  during rollback
- a model check of rollback and discard
- PostgreSQL's own regression and isolation suites run on iheap tables

Results go to `out/`.

`scripts/docker.sh bench` and `scripts/docker.sh olap` run benchmarks that
compare heap with iheap, using the release image
(`scripts/docker.sh image release`).

## Layout

| Path | Contents |
|---|---|
| `src/iheap_*.c` | the iheap table access method |
| `src/dmbtree/` | dmbtree, forked from PostgreSQL's nbtree and amcheck by `scripts/dmbtree_fork.sh` |
| `sql/` | the extension's SQL objects |
| `test/` | regression, isolation and TAP tests, the rollback model, core-suite runs |
| `bench/` | benchmarks: heap vs iheap |
| `docker/`, `scripts/` | the development image and the host-side entry point |
