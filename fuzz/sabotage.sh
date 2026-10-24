#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# `make fuzz-sabotage`: A HARNESS THAT CANNOT FIND A PLANTED BUG FINDS NOTHING.
# For each harness, fuzz/sabotage/<harness>.patch is a one-line bug in the parser
# it covers (a dropped bounds check or an off-by-one). This applies it, rebuilds,
# fuzzes for at most SABOTAGE_TIME seconds from the seed corpus, restores the source
# with `git checkout --`, and is red unless the fuzzer crashed. The source is
# restored on every exit path, and the sanitizer build is rebuilt clean at the end.
#
# Usage: FUZZ_BUILD=build/fuzz MAKE=make sabotage.sh <harness>...
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
B=${FUZZ_BUILD:-build/fuzz}
MAKE=${MAKE:-make}
T=${SABOTAGE_TIME:-30}
OUT=$B/sabotage

patched=
restore() {
	[ -n "$patched" ] && git checkout -- $patched
	patched=
}
trap 'restore' EXIT
trap 'restore; exit 130' INT TERM

if ! git diff --quiet -- src; then
	echo "fuzz-sabotage: src/ has uncommitted changes; refusing to patch over them" >&2
	exit 2
fi

missed=
for h in "$@"; do
	p=fuzz/sabotage/$h.patch
	if [ ! -f "$p" ]; then
		echo "fuzz_$h: no mutant at $p"
		missed="$missed $h"
		continue
	fi
	files=$(git apply --numstat "$p" | awk '{ print $3 }')
	if ! git apply "$p"; then
		echo "fuzz_$h: $p no longer applies"
		missed="$missed $h"
		continue
	fi
	patched=$files
	echo "== fuzz_$h against $p ($(grep '^+[^+]' "$p" | sed 's/^+[[:space:]]*//'))"
	if ! $MAKE --no-print-directory "$B/fuzz_$h" >"$OUT.$h.build.log" 2>&1; then
		mkdir -p "$OUT"
		cat "$OUT.$h.build.log"
		echo "fuzz_$h: the mutant does not build"
		missed="$missed $h"
		restore
		continue
	fi
	rm -rf "$OUT/$h"
	mkdir -p "$OUT/$h/new"
	start=$(date +%s)
	"$B/fuzz_$h" -max_total_time="$T" -max_len=1048576 -rss_limit_mb=2048 -timeout=25 \
		-artifact_prefix="$OUT/$h/" "$OUT/$h/new" "fuzz/corpus/$h" >"$OUT/$h/log" 2>&1
	rc=$?
	took=$(( $(date +%s) - start ))
	restore
	if [ $rc -ne 0 ] && ls "$OUT/$h" | grep -qE '^(crash|leak|oom|timeout)-'; then
		echo "fuzz_$h: mutant found in ${took}s: $(grep -m1 -E '^SUMMARY: ' "$OUT/$h/log")"
	else
		tail -5 "$OUT/$h/log"
		echo "fuzz_$h: MUTANT NOT FOUND in ${T}s -- the harness or its corpus is too weak"
		missed="$missed $h"
	fi
done

# Leave the sanitizer build as the tree is, not as the last mutant was.
$MAKE --no-print-directory fuzz >"$OUT.rebuild.log" 2>&1 || { cat "$OUT.rebuild.log"; exit 1; }

if [ -n "$missed" ]; then
	echo "make fuzz-sabotage: RED --$missed"
	exit 1
fi
echo "make fuzz-sabotage: every mutant was found"
