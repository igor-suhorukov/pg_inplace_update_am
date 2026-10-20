#!/usr/bin/env bash
#
# Host-side entry point.  Every build and every test runs in a throwaway
# container (DESIGN.md §20); this script only invokes docker.
#
#   scripts/docker.sh image [debug|release]
#   scripts/docker.sh build
#   scripts/docker.sh test [regress|isolation|tap|core|all]...
#   scripts/docker.sh shell
#   scripts/docker.sh bench
#   scripts/docker.sh olap [tpch|tpcds|clickbench]...
#
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
IMAGE=pg_inplace_update_am/dev

usage() {
	sed -n '3,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

# run <debug|release> <docker-run-extra-args...> -- <command...>
run() {
	local flavor=$1
	shift
	local extra=()
	while [ $# -gt 0 ] && [ "$1" != -- ]; do
		extra+=("$1")
		shift
	done
	shift
	mkdir -p "$ROOT/out"
	docker run --rm --shm-size=1g "${extra[@]}" \
		-v "$ROOT":/src:ro \
		-v "$ROOT/out":/out \
		"$IMAGE:19-$flavor" "$@"
}

cmd=${1:-}
[ $# -gt 0 ] && shift

case "$cmd" in
	image)
		flavor=${1:-debug}
		docker build \
			--build-arg PG_BUILD="$flavor" \
			--build-arg DEV_UID="$(id -u)" \
			-t "$IMAGE:19-$flavor" \
			-f "$ROOT/docker/Dockerfile" "$ROOT/docker"
		;;
	build)
		run debug -- bash /src/test/run.sh build
		;;
	test)
		run debug -- bash /src/test/run.sh test "${@:-all}"
		;;
	shell)
		run debug -it -- bash
		;;
	bench)
		run release -- bash /src/test/run.sh bench "$@"
		;;
	olap)
		# bench/olap/run.sh: the generated data and the clusters in
		# OLAP_CACHE; ClickBench's checkout and its hits table (a .tsv,
		# CLICKBENCH_TSV inside the container) mounted read-only
		cache=${OLAP_CACHE:-$HOME/.cache/pg_inplace_update_am}
		cb=${CLICKBENCH_DIR:-$ROOT/../../ClickBench}
		hits=${HITS_DIR:-$HOME/.cache/pg_accel/clickbench}
		mkdir -p "$cache"
		extra=(--shm-size=16g -v "$cache":/cache)
		[ -d "$cb" ] && extra+=(-v "$(cd "$cb" && pwd)":/clickbench:ro)
		[ -d "$hits" ] && extra+=(-v "$(cd "$hits" && pwd)":/hits:ro)
		for v in TPCH_SF TPCDS_SF CLICKBENCH_TSV AMS TRIES TIMEOUT QUERIES WORKERS \
			SHARED_BUFFERS WORK_MEM DUCKDB_THREADS; do
			[ -n "${!v:-}" ] && extra+=(-e "$v=${!v}")
		done
		run release "${extra[@]}" -- bash /src/test/run.sh olap "$@"
		;;
	*)
		usage
		exit 2
		;;
esac
