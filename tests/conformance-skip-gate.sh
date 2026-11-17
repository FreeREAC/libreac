#!/bin/sh
# THE PRIVILEGED CI JOB MAY NOT SKIP: tools/skip-gate.sh reads the skip log
# (build/skipped.txt, one test name per line, written by tests/run-test.sh when a test
# exits 77) and fails on any name that is not an allowed one. This is the gate's own test:
# it must pass an empty log, FAIL a missing one, pass a log holding only the allowed skip, and FAIL a log
# holding any other test -- a gate that never fails is not a gate.
set -u
cd "$(dirname "$0")/.." || exit 2
gate=tools/skip-gate.sh
[ -x "$gate" ] || { echo "FAIL: $gate is missing or not executable" >&2; exit 1; }
d=$(mktemp -d) || exit 2
trap 'rm -rf "$d"' EXIT

: > "$d/empty"
"$gate" "$d/empty" >/dev/null 2>&1 || { echo "FAIL: an empty skip log must pass" >&2; exit 1; }
# A missing log is NOT "nothing skipped": make test creates it empty first, so no file means no run.
if "$gate" "$d/missing" >/dev/null 2>&1; then echo "FAIL: a missing skip log must fail the gate" >&2; exit 1; fi

printf 'tools/ops-slugs.sh\ntests/python-binding.sh\n' > "$d/allowed"
"$gate" "$d/allowed" >/dev/null 2>&1 || { echo "FAIL: the allowed skips must pass" >&2; exit 1; }

printf 'tools/ops-slugs.sh\n./test_topo_hears_vlans\n' > "$d/bad"
if "$gate" "$d/bad" >"$d/out" 2>&1; then
	echo "FAIL: a skipped namespace test passed the gate" >&2; exit 1
fi
grep -q 'test_topo_hears_vlans' "$d/out" || { echo "FAIL: the gate did not name the skipped test" >&2; exit 1; }
echo "ok: the skip gate passes an empty log and the allowed skips, and fails a missing log and any other skip"
