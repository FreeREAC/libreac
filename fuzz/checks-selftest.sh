#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# `make fuzz-checks-selftest`: THE FLOOR'S OWN GUARDS, EACH BROKEN ON PURPOSE BY INPUT.
# fuzz/checks.awk and fuzz/coverage.sh decide what counts as a reached check; this feeds
# them the three shapes they once read loosely and requires the strict answer. Needs no
# clang. Sabotage it by undoing a guard in checks.awk or coverage.sh: a case goes red.
#
#   1. a brace-less loop nest above an `if`: the check's guard is the `if`, not a `for`
#   2. a guard line llvm-cov shows with no count: checks.awk prints it as 0
#   3. a blank count in a .checks file: coverage.sh reports the check as not reached
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
red() { echo "fuzz-checks-selftest: RED -- $1"; fail=1; }

# A source whose nest has no comment between the headers and the guard.
cat >"$T/nest.c" <<'SRC'
static void f(int n)
{
	for (int i = 0; i < n; i++)
		for (int j = 0; j < n; j++)
			if (i == j)
				abort();
	switch (n) {
	case 1:
		break;
	default:
		abort();
	}
}
SRC

# 1. The guard of the nested abort is line 5, the `if`.
got=$(awk -v plain=1 -v file=nest.c -f fuzz/checks.awk "$T/nest.c" | awk -F: '$3 == 6 { print $2 }')
[ "$got" = 5 ] || red "the abort at line 6 is guarded by line '$got', not by the if at line 5"
# The default arm's guard is its switch (line 7): the one observable there is that it ran.
got=$(awk -v plain=1 -v file=nest.c -f fuzz/checks.awk "$T/nest.c" | awk -F: '$3 == 11 { print $2 }')
[ "$got" = 7 ] || red "the default abort at line 11 is guarded by line '$got', not by the switch at line 7"

# 2. llvm-cov form: count 4 everywhere, none on the `if` line.
awk '{ printf "%6d|%s|%s\n", NR, (NR == 5 ? "" : "     4"), $0 }' "$T/nest.c" >"$T/show.txt"
got=$(awk -v file=nest.c -f fuzz/checks.awk "$T/show.txt" | awk -F: '$3 == 6 { print "[" $4 "]" }')
[ "$got" = "[0]" ] || red "a guard line with no count reads $got, not [0]"

# 3. coverage.sh on a .checks line with a blank count (a stub checks.awk and llvm-cov).
mkdir -p "$T/tree/fuzz/corpus/knock" "$T/tree/bin"
cp fuzz/coverage.sh "$T/tree/fuzz/"
echo "knock 0" >"$T/tree/fuzz/coverage-floor.txt"
echo seed >"$T/tree/fuzz/corpus/knock/seed"
echo 'BEGIN { print "fuzz/fuzz_knock.c:10:12::1:if (c)" }' >"$T/tree/fuzz/checks.awk"
printf '#!/bin/sh\nexit 0\n' >"$T/tree/bin/llvm-cov"
chmod +x "$T/tree/bin/llvm-cov"
mkdir -p "$T/tree/b"
out=$(cd "$T/tree" && LLVM_COV=bin/llvm-cov sh fuzz/coverage.sh b knock 2>&1 || true)
case $out in
*"no seed reaches (abort at line 12)"*) ;;
*) red "coverage.sh passes a check whose count is blank: $out" ;;
esac
case $out in
*"every check's guard reached"*) red "coverage.sh claims every guard reached over a blank count" ;;
esac

[ "$fail" -eq 0 ] || exit 1
echo "fuzz-checks-selftest: loop nest, default arm and blank count all read strictly"
