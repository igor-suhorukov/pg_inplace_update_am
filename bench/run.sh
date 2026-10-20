#!/usr/bin/env bash
#
# heap vs iheap benchmarks (DESIGN.md §20, §22.3), run inside the release
# image by "scripts/docker.sh bench".  Results: /out/bench/results.csv and
# logs.
#
#   bench/run.sh [duration_seconds] [scale] [clients] [variants] [workloads]
#
# variants and workloads (space-separated lists, default all) rerun some of
# the runs: their rows in results.csv are replaced, the others kept.
#
# Variants (one fresh cluster per workload and variant):
#   heap       heap tables, btree indexes
#   iheap      iheap tables, btree indexes: an update of an indexed column
#              moves the row
#   iheap_dm   iheap tables, dmbtree indexes: it stays in place
#
# Workloads:
#   tpcb         pgbench's TPC-B-like transaction (in-place balance updates)
#   tpcb_hold    the same while a REPEATABLE READ transaction holds a
#                snapshot (no cleanup possible: shows bloat)
#   select       pgbench -S (point lookups through the primary key, which is
#                dmbtree in iheap_dm)
#   idxupd       updates of an indexed column, one secondary index
#   idxupd4      the same with four secondary indexes, one of them changed
#   idxupd_hold  idxupd while a REPEATABLE READ snapshot is held
#   idxmix       idxupd half the time, range reads through that index the
#                other half
#
set -euo pipefail

DURATION=${1:-60}
SCALE=${2:-20}
CLIENTS=${3:-8}
VARIANTS=${4:-heap iheap iheap_dm}
WORKLOADS=${5:-tpcb tpcb_hold select idxupd idxupd4 idxupd_hold idxmix}
ROWS=$(( SCALE * 10000 ))
OUT=${OUT:-/out}/bench
DATA=${BUILD:-/build}/bench_data
PORT=56432
JOBS=$(( CLIENTS < $(nproc) ? CLIENTS : $(nproc) ))

mkdir -p "$OUT"
CSV=$OUT/results.csv
if [ -z "${4:-}${5:-}" ] || [ ! -f "$CSV" ]; then
	echo "workload,variant,tps,latency_ms,table_mb_before,table_mb_after,index_mb_before,index_mb_after,wal_mb,dm_marked" > "$CSV"
fi

want() { [[ " $WORKLOADS " == *" $1 "* ]]; }

psqlq() { psql -X -q -At -p "$PORT" -d postgres "$@"; }

table_am() { [ "$1" = heap ] && echo heap || echo iheap; }
index_am() { [ "$1" = iheap_dm ] && echo dmbtree || echo btree; }

start_cluster() {
	rm -rf "$DATA"
	initdb -D "$DATA" -A trust --no-sync >/dev/null
	cat >> "$DATA/postgresql.conf" <<CONF
shared_preload_libraries = 'pg_inplace_update_am'
shared_buffers = 1GB
max_wal_size = 8GB
checkpoint_timeout = 30min
synchronous_commit = off
max_connections = 100
autovacuum_naptime = 10s
log_min_messages = warning
CONF
	pg_ctl -D "$DATA" -o "-p $PORT" -l "$OUT/server_$1.log" -w start >/dev/null
	psqlq -c 'CREATE EXTENSION pg_inplace_update_am'
}

stop_cluster() {
	pg_ctl -D "$DATA" -m fast -w stop >/dev/null
}

rel_mb() { psqlq -c "SELECT round(pg_relation_size('$1') / 1048576.0, 1)"; }
idx_mb() { psqlq -c "SELECT round(pg_indexes_size('$1') / 1048576.0, 1)"; }
wal_lsn() { psqlq -c "SELECT pg_current_wal_lsn()"; }
wal_mb() { psqlq -c "SELECT round(pg_wal_lsn_diff(pg_current_wal_lsn(), '$1') / 1048576.0, 1)"; }
# delete-marked TIDs left in the table's dmbtree indexes
dm_marked() {
	psqlq -c "SELECT coalesce(sum((dmbtree_index_stats(i.indexrelid)).marked), 0)
		FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
		JOIN pg_am a ON a.oid = c.relam
		WHERE i.indrelid = '$1'::regclass AND a.amname = 'dmbtree'"
}

# hold a REPEATABLE READ snapshot for the next run
hold_snapshot() {
	( psql -X -q -p "$PORT" -d postgres -c "BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class; SELECT pg_sleep($DURATION + 5); COMMIT;" >/dev/null 2>&1 & )
	sleep 1
}

# run_pgbench <workload> <variant> <table> <pgbench args...>
run_pgbench() {
	local tag=$1 variant=$2 table=$3
	shift 3
	local before ibefore lsn out tps lat
	before=$(rel_mb "$table")
	ibefore=$(idx_mb "$table")
	lsn=$(wal_lsn)
	out=$(pgbench -n -p "$PORT" -c "$CLIENTS" -j "$JOBS" -T "$DURATION" "$@" postgres 2>&1)
	echo "$out" > "$OUT/${tag}_${variant}.log"
	tps=$(echo "$out" | sed -nE 's/^tps = ([0-9.]+).*/\1/p' | tail -1)
	lat=$(echo "$out" | sed -nE 's/^latency average = ([0-9.]+) ms/\1/p')
	sed -i "/^$tag,$variant,/d" "$CSV"
	echo "$tag,$variant,$tps,$lat,$before,$(rel_mb "$table"),$ibefore,$(idx_mb "$table"),$(wal_mb "$lsn"),$(dm_marked "$table")" | tee -a "$CSV"
}

# pgbench's tables; in iheap_dm the primary keys are dmbtree indexes
pgbench_init() {
	PGOPTIONS="-c default_table_access_method=$(table_am "$1") -c iheap.unique_index_am=$(index_am "$1")" \
		pgbench -i -q -p "$PORT" -s "$SCALE" postgres >/dev/null 2>&1
	psqlq -c 'VACUUM ANALYZE'
}

# table t with secondary indexes on k (and on a, b, c with "4")
idx_table() {
	local tam idxam
	tam=$(table_am "$1")
	idxam=$(index_am "$1")
	# one -c per statement: VACUUM cannot run in the implicit transaction
	# of a multi-statement string
	psqlq -c "CREATE TABLE t (id int PRIMARY KEY, k int, a int, b int, c int, v int, pad text) USING $tam" \
		-c "CREATE INDEX t_k ON t USING $idxam (k)"
	if [ "${2:-1}" = 4 ]; then
		psqlq -c "CREATE INDEX t_a ON t USING $idxam (a)" \
			-c "CREATE INDEX t_b ON t USING $idxam (b)" \
			-c "CREATE INDEX t_c ON t USING $idxam (c)"
	fi
	psqlq -c "INSERT INTO t SELECT g, g, g, g, g, 0, repeat('x', 80) FROM generate_series(1, $ROWS) g" \
		-c "VACUUM ANALYZE t"
}

cat > "$OUT/idxupd.sql" <<SQL
\set id random(1, $ROWS)
UPDATE t SET k = k + 1, v = v + 1 WHERE id = :id;
SQL
cat > "$OUT/idxread.sql" <<SQL
\set k random(1, $ROWS)
SELECT count(*), sum(v) FROM t WHERE k BETWEEN :k AND :k + 9;
SQL

for variant in $VARIANTS; do
	# --- TPC-B-like, without and with a snapshot held (no secondary index:
	# dmbtree changes nothing here; tpcb_hold continues from tpcb) ---------
	if [ "$variant" != iheap_dm ] && { want tpcb || want tpcb_hold; }; then
		start_cluster "tpcb_$variant"
		pgbench_init "$variant"
		run_pgbench tpcb "$variant" pgbench_accounts
		if want tpcb_hold; then
			psqlq -c 'VACUUM'
			hold_snapshot
			run_pgbench tpcb_hold "$variant" pgbench_accounts
			sleep 5
		fi
		stop_cluster
	fi

	# --- read-only point lookups ---------------------------------------------
	if want select; then
		start_cluster "select_$variant"
		pgbench_init "$variant"
		run_pgbench select "$variant" pgbench_accounts -S
		stop_cluster
	fi

	# --- indexed-column updates ----------------------------------------------
	if want idxupd; then
		start_cluster "idxupd_$variant"
		idx_table "$variant"
		run_pgbench idxupd "$variant" t -f "$OUT/idxupd.sql"
		stop_cluster
	fi

	if want idxupd4; then
		start_cluster "idxupd4_$variant"
		idx_table "$variant" 4
		run_pgbench idxupd4 "$variant" t -f "$OUT/idxupd.sql"
		stop_cluster
	fi

	if want idxupd_hold; then
		start_cluster "idxupd_hold_$variant"
		idx_table "$variant"
		hold_snapshot
		run_pgbench idxupd_hold "$variant" t -f "$OUT/idxupd.sql"
		sleep 5
		stop_cluster
	fi

	if want idxmix; then
		start_cluster "idxmix_$variant"
		idx_table "$variant"
		run_pgbench idxmix "$variant" t -f "$OUT/idxupd.sql@1" -f "$OUT/idxread.sql@1"
		stop_cluster
	fi
done

echo
column -s, -t "$CSV" 2>/dev/null || cat "$CSV"
