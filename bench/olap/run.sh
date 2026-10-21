#!/usr/bin/env bash
#
# Analytical benchmarks, heap vs iheap: TPC-H, TPC-DS and ClickBench, run
# inside the release image by "scripts/docker.sh olap".
#
#   bench/olap/run.sh [tpch|tpcds|clickbench]...     (default: all three)
#
# Each access method gets a fresh cluster per benchmark: the tables are
# created with it, loaded with COPY, indexed, VACUUM ANALYZEd and
# checkpointed; then every query runs TRIES times, the first after a
# restart with the cluster's files dropped from the page cache (cold), the
# best of the others being the hot time (ClickBench's method).  The answer
# of each access method's last run is compared with the first's (heap's).
#
# Settings (environment):
#   TPCH_SF, TPCDS_SF  scale factors (10, 10)
#   CLICKBENCH_TSV     the hits table's rows: ClickBench's hits.tsv or a
#                      subset of it (/hits/hits_10m.tsv)
#   AMS                access methods, the first being the reference ("heap iheap")
#   TRIES              runs of each query (3)
#   TIMEOUT            statement_timeout in seconds (180); a query whose
#                      first run fails or times out is not run again
#   QUERIES            only these queries, e.g. "q01 q17" (all)
#   WORKERS            max_parallel_workers_per_gather (8)
#   SHARED_BUFFERS, WORK_MEM  (8GB, 128MB)
#
# Data: TPC-H and TPC-DS are generated once by DuckDB's tpch and tpcds
# extensions (olap.py tpcgen) into /cache/data/<kind>-sf<sf>; ClickBench's
# table definition and queries come from its checkout at /clickbench.  The
# clusters live in /cache/pgdata.  Results: /out/olap/<bench>/ and
# /out/olap/summary.md.
#
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OUT=${OUT:-/out}/olap
CACHE=${CACHE:-/cache}
PGDATA_DIR=$CACHE/pgdata
PORT=56433
TPCH_SF=${TPCH_SF:-10}
TPCDS_SF=${TPCDS_SF:-10}
CLICKBENCH_DIR=${CLICKBENCH_DIR:-/clickbench}
CLICKBENCH_TSV=${CLICKBENCH_TSV:-/hits/hits_10m.tsv}
read -ra AMS <<<"${AMS:-heap iheap}"
TRIES=${TRIES:-3}
TIMEOUT=${TIMEOUT:-180}
QUERIES=${QUERIES:-}
WORKERS=${WORKERS:-8}
SHARED_BUFFERS=${SHARED_BUFFERS:-8GB}
WORK_MEM=${WORK_MEM:-128MB}
DUCKDB_PY=/opt/duckdb/bin/python

benches=("$@")
[ ${#benches[@]} -eq 0 ] && benches=(tpch tpcds clickbench)

log() { printf '%s  %s\n' "$(date +%T)" "$*"; }
now() { date +%s.%N; }
since() { awk -v a="$1" -v b="$(now)" 'BEGIN { printf "%.1f", b - a }'; }
psqlq() { psql -X -q -At -v ON_ERROR_STOP=1 -p "$PORT" -d bench "$@"; }

server_conf() {
	cat <<CONF
shared_preload_libraries = 'pg_inplace_update_am'
max_connections = 20
shared_buffers = $SHARED_BUFFERS
work_mem = $WORK_MEM
maintenance_work_mem = 2GB
effective_cache_size = 32GB
max_wal_size = 32GB
checkpoint_timeout = 1h
synchronous_commit = off
max_worker_processes = 24
max_parallel_workers = 16
max_parallel_workers_per_gather = $WORKERS
max_parallel_maintenance_workers = 4
random_page_cost = 1.1
autovacuum = off
jit = off
log_min_messages = warning
# btree under both access methods: the benchmarks compare table AMs
iheap.default_index_am = btree
CONF
}

start_cluster() {
	rm -rf "$PGDATA_DIR"
	initdb -D "$PGDATA_DIR" -A trust --no-sync --locale=C --encoding=UTF8 >/dev/null
	server_conf >>"$PGDATA_DIR/postgresql.conf"
	pg_ctl -D "$PGDATA_DIR" -o "-p $PORT" -l "$SERVER_LOG" -w start >/dev/null
	psql -X -q -p "$PORT" -d postgres -c 'CREATE DATABASE bench'
	psqlq -c 'CREATE EXTENSION pg_inplace_update_am'
}

stop_cluster() {
	pg_ctl -D "$PGDATA_DIR" -m fast -w stop >/dev/null
}

# restart with nothing of the cluster cached: shared buffers are emptied by
# the restart, the shutdown checkpoint leaves the files clean, and their
# pages are dropped from the kernel's cache
restart_cold() {
	stop_cluster
	python3 "$HERE/olap.py" evict "$PGDATA_DIR"
	pg_ctl -D "$PGDATA_DIR" -o "-p $PORT" -l "$SERVER_LOG" -w start >/dev/null
}

# every table of the benchmark must have been created with the access method
check_am() {
	local am=$1 n bad
	n=$(psqlq -c "SELECT count(*) FROM pg_class WHERE relkind = 'r' AND relnamespace = 'public'::regnamespace")
	bad=$(psqlq -c "SELECT string_agg(c.relname, ' ') FROM pg_class c JOIN pg_am a ON a.oid = c.relam
		WHERE c.relkind = 'r' AND c.relnamespace = 'public'::regnamespace AND a.amname <> '$am'")
	if [ "$n" -eq 0 ] || [ -n "$bad" ]; then
		echo "tables not created with $am: ${bad:-none created}" >&2
		exit 1
	fi
}

# table MB (main fork, which holds iheap's undo too), TOAST MB, index MB
sizes() {
	psqlq -F $'\t' -c "SELECT round(sum(pg_relation_size(c.oid)) / 1048576.0, 1),
		round(sum(COALESCE(pg_total_relation_size(NULLIF(c.reltoastrelid, 0)), 0)) / 1048576.0, 1),
		round(sum(pg_indexes_size(c.oid)) / 1048576.0, 1)
		FROM pg_class c WHERE c.relkind = 'r' AND c.relnamespace = 'public'::regnamespace"
}

# load <am> <schema.sql> <indexes.sql or ""> <copy statements...>
load() {
	local am=$1 schema=$2 indexes=$3 t0 lsn copy_s index_s=0 vacuum_s loaded
	shift 3
	PGOPTIONS="-c default_table_access_method=$am" psqlq -f "$schema"
	check_am "$am"
	lsn=$(psqlq -c 'SELECT pg_current_wal_lsn()')
	t0=$(now)
	for stmt in "$@"; do
		psqlq -c "$stmt"
	done
	copy_s=$(since "$t0")
	loaded=$(sizes | cut -f1)
	if [ -n "$indexes" ]; then
		t0=$(now)
		psqlq -f "$indexes"
		index_s=$(since "$t0")
	fi
	t0=$(now)
	psqlq -c 'VACUUM ANALYZE'
	vacuum_s=$(since "$t0")
	psqlq -c 'CHECKPOINT'
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$am" "$copy_s" "$index_s" "$vacuum_s" \
		"$(psqlq -c "SELECT round(pg_wal_lsn_diff(pg_current_wal_lsn(), '$lsn') / 1048576.0, 1)")" \
		"$loaded" "$(sizes)" >>"$BDIR/load.tsv"
	log "$BENCH $am: COPY ${copy_s} s, indexes ${index_s} s, VACUUM ANALYZE ${vacuum_s} s; $(tail -1 "$BDIR/load.tsv" | cut -f5- | tr '\t' ' ') (WAL, MB after COPY, table, TOAST, index MB)"
	if [ "$am" = iheap ]; then
		psqlq -F $'\t' -P footer=off -c "SELECT c.relname, s.* FROM pg_class c, iheap_relation_stats(c.oid) s
			WHERE c.relkind = 'r' AND c.relnamespace = 'public'::regnamespace ORDER BY c.relname" \
			>"$BDIR/iheap_relation_stats.tsv"
	fi
}

# run_query <am> <name> <file>
run_query() {
	local am=$1 q=$2 f=$3 tr rc out ms st t0
	restart_cold
	for tr in $(seq 1 "$TRIES"); do
		t0=$(now)
		rc=0
		out=$(PGOPTIONS="-c statement_timeout=${TIMEOUT}s" psql -X -q -At -v ON_ERROR_STOP=1 \
			-p "$PORT" -d bench -o "$BDIR/$am/$q.out" -c '\timing on' -f "$f" \
			2>"$BDIR/$am/$q.err") || rc=$?
		if [ $rc -eq 0 ]; then
			st=ok
			ms=$(echo "$out" | sed -nE 's/^Time: ([0-9.]+) ms.*/\1/p' | awk '{ s += $1 } END { printf "%.3f", s }')
		else
			grep -q 'statement timeout' "$BDIR/$am/$q.err" && st=timeout || st=error
			ms=$(awk -v s="$(since "$t0")" 'BEGIN { printf "%.3f", s * 1000 }')
		fi
		printf '%s\t%s\t%s\t%s\t%s\n' "$am" "$q" "$tr" "$ms" "$st" >>"$BDIR/results.tsv"
		log "$BENCH $am $q try $tr: $ms ms $st"
		[ "$st" = ok ] || break
	done
	[ -s "$BDIR/$am/$q.err" ] || rm -f "$BDIR/$am/$q.err"
}

selected() {
	[ -z "$QUERIES" ] && return 0
	case " $QUERIES " in *" $1 "*) return 0 ;; esac
	return 1
}

# --- the benchmarks' data -----------------------------------------------

# tpc_data <h|ds> <sf>: generated once, kept in the cache
tpc_data() {
	local kind=$1 sf=$2 dir=$CACHE/data/tpc$1-sf$2 t0
	if [ ! -f "$dir/.done" ]; then
		log "generating TPC-$([ "$kind" = h ] && echo H || echo DS) at scale factor $sf with DuckDB"
		rm -rf "$dir"
		t0=$(now)
		"$DUCKDB_PY" "$HERE/olap.py" tpcgen "$kind" "$sf" "$dir"
		touch "$dir/.done"
		log "generated in $(since "$t0") s: $(du -sh "$dir/data" | cut -f1)"
	fi
	DATA=$dir
}

tpc_copies() {
	local f
	for f in "$DATA"/data/*.csv; do
		echo "\\copy $(basename "$f" .csv) FROM '$f' WITH (FORMAT csv, DELIMITER '|', NULL '')"
	done
}

bench_tpch() {
	tpc_data h "$TPCH_SF"
	echo "scale factor $TPCH_SF" >"$BDIR/scale.txt"
	QDIR=$DATA/q
	SCHEMA=$DATA/schema.sql
	INDEXES=$HERE/tpch_indexes.sql
	mapfile -t COPIES < <(tpc_copies)
}

bench_tpcds() {
	tpc_data ds "$TPCDS_SF"
	echo "scale factor $TPCDS_SF" >"$BDIR/scale.txt"
	QDIR=$DATA/q
	SCHEMA=$DATA/schema.sql
	INDEXES=$HERE/tpcds_indexes.sql
	mapfile -t COPIES < <(tpc_copies)
}

# ClickBench's PostgreSQL table and queries, read from its checkout (the
# benchmark's license does not allow copying them into this tree); its load
# script's COPY FREEZE is not used, as for the TPC benchmarks
bench_clickbench() {
	local i=0 line
	[ -f "$CLICKBENCH_DIR/postgresql/queries.sql" ] || { echo "no ClickBench checkout at $CLICKBENCH_DIR" >&2; exit 1; }
	[ -f "$CLICKBENCH_TSV" ] || { echo "no $CLICKBENCH_TSV" >&2; exit 1; }
	echo "$(basename "$CLICKBENCH_TSV"), $(git -C "$CLICKBENCH_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown checkout)" >"$BDIR/scale.txt"
	QDIR=$CACHE/data/clickbench/q
	rm -rf "$QDIR"
	mkdir -p "$QDIR"
	while IFS= read -r line; do
		[ -n "$line" ] || continue
		i=$((i + 1))
		printf '%s\n' "$line" >"$QDIR/$(printf 'q%02d' "$i").sql"
	done <"$CLICKBENCH_DIR/postgresql/queries.sql"
	SCHEMA=$CLICKBENCH_DIR/postgresql/create.sql
	INDEXES=
	COPIES=("\\copy hits FROM '$CLICKBENCH_TSV'")
}

# --- main ---------------------------------------------------------------

mkdir -p "$OUT"
{
	echo "date: $(date -u +%FT%TZ)"
	echo "PostgreSQL: $(cut -c1-12 /usr/src/postgres/.pg_commit 2>/dev/null) ($(cat /opt/pg19/.pg_build 2>/dev/null) build)"
	echo "iheap sources: $(cat "$HERE"/../../src/*.c "$HERE"/../../src/*.h | md5sum | cut -c1-12) (md5 of src/)"
	echo "host: $(nproc) CPUs, $(awk '/MemTotal/ { printf "%.0f GB", $2 / 1048576 }' /proc/meminfo)"
	echo "tries: $TRIES (first cold), statement_timeout: ${TIMEOUT}s"
	server_conf | grep -v '^shared_preload'
} >"$OUT/settings.txt"

for BENCH in "${benches[@]}"; do
	case "$BENCH" in
		tpch|tpcds|clickbench) ;;
		*) echo "unknown benchmark: $BENCH" >&2; exit 2 ;;
	esac
	BDIR=$OUT/$BENCH
	rm -rf "$BDIR"
	mkdir -p "$BDIR"
	"bench_$BENCH"
	printf 'am\tcopy_s\tindex_s\tvacuum_s\twal_load_mb\ttable_mb_loaded\ttable_mb\ttoast_mb\tindex_mb\n' >"$BDIR/load.tsv"
	: >"$BDIR/results.tsv"
	for am in "${AMS[@]}"; do
		mkdir -p "$BDIR/$am"
		SERVER_LOG=$BDIR/server_$am.log
		log "$BENCH $am: loading"
		start_cluster
		load "$am" "$SCHEMA" "$INDEXES" "${COPIES[@]}"
		for f in "$QDIR"/*.sql; do
			q=$(basename "$f" .sql)
			if selected "$q"; then
				run_query "$am" "$q" "$f"
			fi
		done
		stop_cluster
		if grep -E 'PANIC|TRAP|terminated by signal|server process .* was terminated' "$SERVER_LOG"; then
			log "$BENCH $am: the server log has crashes (above)"
		fi
	done
	rm -rf "$PGDATA_DIR"
done

python3 "$HERE/olap.py" report "$OUT"
