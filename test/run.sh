#!/usr/bin/env bash
#
# In-container driver (DESIGN.md §20).  Never run on the host.
#
#   run.sh build                    copy /src to /build, compile with -Werror
#   run.sh test [suite...]          build, install, run suites:
#                                   regress isolation tap core core-dm all
#                                   core:isolation/<test>,regress/<test>
#   run.sh bench [args]             benchmarks (release image)
#   run.sh olap [bench...]          analytical benchmarks (release image)
#
# /src is the read-only source tree, /out the output directory; results of
# suite S go to /out/S.
#
set -euo pipefail

if [ ! -f /.dockerenv ] && [ -z "${IHEAP_IN_CONTAINER:-}" ]; then
	echo "run.sh must run inside the development container (scripts/docker.sh)" >&2
	exit 2
fi

SRC=${SRC:-/src}
BUILD=${BUILD:-/build}
OUT=${OUT:-/out}
PG_CONFIG=${PG_CONFIG:-pg_config}
BINDIR=$("$PG_CONFIG" --bindir)
PGXS_DIR=$(cd "$(dirname "$("$PG_CONFIG" --pgxs)")/../.." && pwd)
PG_REGRESS=$PGXS_DIR/src/test/regress/pg_regress
PG_ISOLATION_REGRESS=$PGXS_DIR/src/test/isolation/pg_isolation_regress
PGSRC=/usr/src/postgres
JOBS=$(nproc)

log() { printf '\n=== %s\n' "$*"; }

sync_tree() {
	mkdir -p "$BUILD"
	# keep object files between runs in the same container; sources are
	# refreshed from /src every time
	tar -C "$SRC" --exclude=./out -cf - . | tar -C "$BUILD" -xf -
}

do_build() {
	log "build"
	sync_tree
	make -C "$BUILD" -j"$JOBS" -k -s COPT='-Werror'
}

do_install() {
	log "install"
	make -C "$BUILD" -s install
}

# pg_regress / pg_isolation_regress against a temporary instance
run_pgregress() {
	local bin=$1 suite=$2 schedule=$3
	shift 3
	local out=$OUT/$suite
	rm -rf "$out"
	mkdir -p "$out"
	# shellcheck disable=SC2046
	"$bin" \
		--temp-instance="$BUILD/tmp_$suite" \
		--temp-config="$BUILD/test/iheap.conf" \
		--inputdir="$BUILD/test" \
		--outputdir="$out" \
		--bindir="$BINDIR" \
		--load-extension=pg_inplace_update_am \
		--max-connections=20 \
		"$@" \
		$(grep -v '^#' "$BUILD/test/$schedule")
}

suite_regress() {
	log "regress"
	run_pgregress "$PG_REGRESS" regress regress_schedule \
		--dbname=contrib_regression
}

suite_isolation() {
	log "isolation"
	run_pgregress "$PG_ISOLATION_REGRESS" isolation isolation_schedule \
		--dbname=isolation_regression
}

suite_tap() {
	log "tap"
	local out=$OUT/tap
	rm -rf "$out" "$BUILD/tmp_tap"
	mkdir -p "$out" "$BUILD/tmp_tap"
	(
		cd "$BUILD"
		TESTLOGDIR="$out" \
		TESTDATADIR="$BUILD/tmp_tap" \
		PATH="$BINDIR:$PATH" \
		PG_REGRESS="$PG_REGRESS" \
		prove -I "$PGXS_DIR/src/test/perl" -I "$BUILD/test/t" \
			${PROVE_FLAGS:-} test/t/*.pl
	)
}

# PostgreSQL's own suites with iheap as the default table AM.  The
# extension is installed into template1 so that databases created by the
# tests have the access method too.
#
# Without an argument every test runs and the result is compared with
# test/core/expected_failures.  "core:regress/<name>,isolation/<name>,..."
# runs only the named tests (regress ones after test_setup) into
# /out/core-sel, for looking at one difference at a time.
#
# CORE_FLAVOR=dm (suite "core-dm"): the same with every index of an iheap
# table a dmbtree index (test/core/core-dm.conf), into /out/core-dm, and
# compared with test/core/expected_failures plus expected_failures_dm.
suite_core() {
	local sel=${1:-}
	local flavor=${CORE_FLAVOR:-}
	local out=$OUT/core${flavor:+-$flavor} data=$BUILD/tmp_core/data port=55432
	local regress_args=() isolation_args=() t items
	if [ -n "$sel" ]; then
		out=$OUT/core${flavor:+-$flavor}-sel
		IFS=, read -ra items <<<"$sel"
		for t in "${items[@]}"; do
			case "$t" in
				regress/?*) regress_args+=("${t#regress/}") ;;
				isolation/?*) isolation_args+=("${t#isolation/}") ;;
				*)
					echo "core test must be regress/<name> or isolation/<name>: $t" >&2
					return 2
					;;
			esac
		done
		[ ${#regress_args[@]} -gt 0 ] && regress_args=(test_setup "${regress_args[@]}")
	else
		regress_args=(--schedule="$PGSRC/src/test/regress/parallel_schedule"
			--max-concurrent-tests=20)
		isolation_args=(--schedule="$PGSRC/src/test/isolation/isolation_schedule")
	fi
	log "core${flavor:+-$flavor}${sel:+ ($sel)}"
	rm -rf "$out" "$BUILD/tmp_core"
	mkdir -p "$out" "$BUILD/tmp_core"
	initdb -D "$data" --no-sync -A trust -U pg >"$out/initdb.log" 2>&1
	cat "$BUILD/test/iheap.conf" "$BUILD/test/core/core.conf" >>"$data/postgresql.conf"
	if [ -n "$flavor" ]; then
		cat "$BUILD/test/core/core-$flavor.conf" >>"$data/postgresql.conf"
	fi
	pg_ctl -D "$data" -o "-p $port" -l "$out/postmaster.log" -w start
	trap 'pg_ctl -D "'"$data"'" -m immediate -w stop >/dev/null 2>&1 || true' RETURN
	for db in template1 postgres; do
		psql -X -q -p "$port" -d "$db" -c 'CREATE EXTENSION pg_inplace_update_am'
	done
	psql -X -q -p "$port" -d postgres \
		-c "ALTER SYSTEM SET default_table_access_method = 'iheap'" \
		-c 'SELECT pg_reload_conf()'
	local rc=0
	if [ ${#regress_args[@]} -gt 0 ]; then
		"$PG_REGRESS" \
			--port="$port" --user=pg \
			--inputdir="$PGSRC/src/test/regress" \
			--outputdir="$out/regress" \
			--bindir="$BINDIR" \
			--dlpath="$PGSRC/src/test/regress" \
			--expecteddir="$PGSRC/src/test/regress" \
			--load-extension=pg_inplace_update_am \
			"${regress_args[@]}" || rc=$?
	fi
	if [ ${#isolation_args[@]} -gt 0 ]; then
		"$PGSRC/src/test/isolation/pg_isolation_regress" \
			--port="$port" --user=pg \
			--inputdir="$PGSRC/src/test/isolation" \
			--outputdir="$out/isolation" \
			--bindir="$BINDIR" \
			--load-extension=pg_inplace_update_am \
			"${isolation_args[@]}" || rc=$?
	fi
	if [ -z "$sel" ]; then
		# heap's expected output differs in known ways (row order, plans,
		# page layout): the verdict is whether every difference is known
		rc=0
		bash "$BUILD/test/core/compare.sh" "$out" ${flavor:+"$flavor"} || rc=$?
	fi
	return $rc
}

do_test() {
	local suites=("$@") rc=0
	[ ${#suites[@]} -eq 0 ] && suites=(all)
	if [ "${suites[0]}" = all ]; then
		suites=(regress isolation tap)
	fi
	do_build
	do_install
	for s in "${suites[@]}"; do
		case "$s" in
			regress|isolation|tap|core) "suite_$s" || rc=1 ;;
			core:*) suite_core "${s#core:}" || rc=1 ;;
			core-dm) CORE_FLAVOR=dm suite_core || rc=1 ;;
			core-dm:*) CORE_FLAVOR=dm suite_core "${s#core-dm:}" || rc=1 ;;
			*) echo "unknown suite: $s" >&2; return 2 ;;
		esac
	done
	if [ $rc -ne 0 ]; then
		log "FAILED (see out/<suite>/)"
	else
		log "all suites passed"
	fi
	return $rc
}

cmd=${1:-}
[ $# -gt 0 ] && shift
case "$cmd" in
	build) do_build ;;
	test) do_test "$@" ;;
	bench)
		do_build
		do_install
		bash "$BUILD/bench/run.sh" "$@"
		;;
	olap)
		do_build
		do_install
		bash "$BUILD/bench/olap/run.sh" "$@"
		;;
	*)
		sed -n '3,13p' "$0" | sed 's/^# \{0,1\}//'
		exit 2
		;;
esac
