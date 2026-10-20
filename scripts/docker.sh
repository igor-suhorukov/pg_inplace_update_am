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
#
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
IMAGE=pg_inplace_update_am/dev

usage() {
	sed -n '3,11p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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
	docker run --rm "${extra[@]}" \
		--shm-size=1g \
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
	*)
		usage
		exit 2
		;;
esac
