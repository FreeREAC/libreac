#!/bin/sh
# THE FACTS DRIFT GATE MUST BE ABLE TO FAIL INSTEAD OF SKIPPING.
#
# `make facts-drift-check` prints "skipping" when reac-protocol is not reachable, which is
# right for a release-tarball build and wrong for CI: a CI run that skips compares nothing,
# and the four generated fact headers can fall behind the declaration with the run green.
# CI therefore sets REAC_REQUIRE_FACTS_GATE=1, which turns the skip into a failure.
#
# Two arms, both with the schema pointed at a directory that has none:
#   - REAC_REQUIRE_FACTS_GATE=1  -> the gate must FAIL, naming the missing schema;
#   - not set                    -> the gate must still skip (standalone builds).
set -u
cd "$(dirname "$0")/.." || exit 2
none=$(mktemp -d) || exit 2
trap 'rm -rf "$none"' EXIT

out=$(REAC_PROTOCOL="$none" REAC_REQUIRE_FACTS_GATE=1 make --no-print-directory facts-drift-check 2>&1)
rc=$?
if [ "$rc" -eq 0 ]; then
	echo "FAIL: facts-drift-check passed with REAC_REQUIRE_FACTS_GATE=1 and no schema; CI would be green on a skipped gate" >&2
	echo "$out" >&2
	exit 1
fi
echo "$out" | grep -q 'not reachable' || {
	echo "FAIL: the required gate failed without saying the schema is not reachable:" >&2
	echo "$out" >&2
	exit 1
}

out=$(REAC_PROTOCOL="$none" make --no-print-directory facts-drift-check 2>&1) || {
	echo "FAIL: a standalone build (gate not required) must skip, not fail:" >&2
	echo "$out" >&2
	exit 1
}
echo "$out" | grep -q 'skipping' || { echo "FAIL: the standalone skip no longer says so: $out" >&2; exit 1; }
echo "ok: the required facts gate fails when the schema is missing, and a standalone build still skips"
