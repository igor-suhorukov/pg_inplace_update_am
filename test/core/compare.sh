#!/usr/bin/env bash
#
# Summarize the core-suite run (test/run.sh core): which tests failed, and
# whether that matches the known differences in test/core/expected_failures
# (one test name per line, "regress/<name>" or "isolation/<name>", with a
# comment saying why the output legitimately differs for iheap).
#
set -uo pipefail

out=$1
flavor=${2:-}
known=$(dirname "$0")/expected_failures

failed=$(
	for suite in regress isolation; do
		f=$out/$suite/regression.out
		[ -f "$f" ] || continue
		grep -E '^(not ok|# .* failed)' "$f" >/dev/null 2>&1 || true
		sed -nE 's/^not ok +[0-9]+ +[-+] +([^ ]+).*/\1/p' "$f" | sed "s|^|$suite/|"
	done | sort -u
)
# a flavor (core-dm) adds the differences of its own to the table AM's
expected=$(cat "$known" ${flavor:+"$known"_"$flavor"} 2>/dev/null | grep -v '^#' |
	sed 's/[[:space:]].*//' | grep -v '^$' | sort -u)

new=$(comm -23 <(echo "$failed" | grep -v '^$') <(echo "$expected" | grep -v '^$'))
fixed=$(comm -13 <(echo "$failed" | grep -v '^$') <(echo "$expected" | grep -v '^$'))

echo "core suite: $(echo "$failed" | grep -vc '^$') failing tests, $(echo "$expected" | grep -vc '^$') known"
if [ -n "$fixed" ]; then
	echo "known failures that now pass (remove from expected_failures):"
	echo "$fixed" | sed 's/^/  /'
fi
if [ -n "$new" ]; then
	echo "unexpected failures:"
	echo "$new" | sed 's/^/  /'
	exit 1
fi
exit 0
