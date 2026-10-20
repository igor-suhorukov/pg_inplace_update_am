#!/usr/bin/env bash
#
# heap vs iheap benchmarks (DESIGN.md §20), run inside the release image by
# "scripts/docker.sh bench".  Results: /out/bench/results.csv and logs.
#
#   bench/run.sh [duration_seconds] [scale] [clients]
#
# Workloads, each run once for heap and once for iheap on a fresh cluster:
#   tpcb       pgbench's TPC-B-like transaction (in-place balance updates)
#   tpcb_hold  the same while a REPEATABLE READ transaction holds a snapshot
#              (no cleanup possible: shows bloat)
#   idxupd     updates of an indexed column (rows must move in iheap)
#   select     pgbench -S (read-only point lookups)
#
set -euo pipefail

DURATION=${1:-60}
SCALE=${2:-20}
CLIENTS=${3:-8}
OUT=${OUT:-/out}/bench
DATA=${BUILD:-/build}/bench_data
PORT=56432
JOBS=$(( CLIENTS < $(nproc) ? CLIENTS : $(nproc) ))

mkdir -p "$OUT"
CSV=$OUT/results.csv
echo "workload,am,tps,latency_ms,table_mb_before,table_mb_after,index_mb_after,wal_mb" > "$CSV"

psqlq() { psql -X -q -At -p "$PORT" -d postgres "$@"; }

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

# run_pgbench <tag> <am> <table> <pgbench args...>
run_pgbench() {
	local tag=$1 am=$2 table=$3
	shift 3
	local before lsn out tps lat
	before=$(rel_mb "$table")
	lsn=$(wal_lsn)
	out=$(pgbench -n -p "$PORT" -c "$CLIENTS" -j "$JOBS" -T "$DURATION" "$@" postgres 2>&1)
	echo "$out" > "$OUT/${tag}_${am}.log"
	tps=$(echo "$out" | sed -nE 's/^tps = ([0-9.]+).*/\1/p' | tail -1)
	lat=$(echo "$out" | sed -nE 's/^latency average = ([0-9.]+) ms/\1/p')
	echo "$tag,$am,$tps,$lat,$before,$(rel_mb "$table"),$(idx_mb "$table"),$(wal_mb "$lsn")" | tee -a "$CSV"
}

for am in heap iheap; do
	# --- TPC-B-like -------------------------------------------------------
	start_cluster "tpcb_$am"
	PGOPTIONS="-c default_table_access_method=$am" pgbench -i -q -p "$PORT" -s "$SCALE" postgres >/dev/null 2>&1
	psqlq -c 'VACUUM ANALYZE'
	run_pgbench tpcb "$am" pgbench_accounts

	# --- TPC-B-like with a snapshot held open -------------------------------
	psqlq -c 'VACUUM'
	( psql -X -q -p "$PORT" -d postgres -c "BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pgbench_branches; SELECT pg_sleep($DURATION + 5); COMMIT;" >/dev/null 2>&1 & )
	sleep 1
	run_pgbench tpcb_hold "$am" pgbench_accounts
	sleep 5
	stop_cluster

	# --- indexed-column updates --------------------------------------------
	start_cluster "idxupd_$am"
	# one -c per statement: VACUUM cannot run in the implicit transaction
	# of a multi-statement string
	psqlq -c "CREATE TABLE t (id int PRIMARY KEY, k int, v int, pad text) USING $am" \
		-c "CREATE INDEX t_k ON t (k)" \
		-c "INSERT INTO t SELECT g, g, 0, repeat('x', 80) FROM generate_series(1, $SCALE * 10000) g" \
		-c "VACUUM ANALYZE t"
	cat > "$OUT/idxupd.sql" <<SQL
\set id random(1, $SCALE * 10000)
UPDATE t SET k = k + 1, v = v + 1 WHERE id = :id;
SQL
	run_pgbench idxupd "$am" t -f "$OUT/idxupd.sql"
	stop_cluster

	# --- read-only point lookups -------------------------------------------
	start_cluster "select_$am"
	PGOPTIONS="-c default_table_access_method=$am" pgbench -i -q -p "$PORT" -s "$SCALE" postgres >/dev/null 2>&1
	psqlq -c 'VACUUM ANALYZE'
	run_pgbench select "$am" pgbench_accounts -S
	stop_cluster
done

echo
column -s, -t "$CSV" 2>/dev/null || cat "$CSV"
