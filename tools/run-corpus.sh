#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# Decode the whole FreeREAC capture corpus with this build of libreac and refuse
# a regression against tests/corpus-baseline.txt.
#
# The corpus is NOT in this repo — it is FreeREAC's private capture set — so this
# is a dev-only gate, like a rig check, and the committed baseline records the
# COUNTS rather than the bytes. Point it at the corpus:
#
#   tools/run-corpus.sh                          # compare against the baseline
#   tools/run-corpus.sh --captures DIR           # elsewhere
#   tools/run-corpus.sh --write-baseline         # record this run as the baseline
#   tools/run-corpus.sh --self-test              # prove the checker can go red
#
# WHAT IT GATES. Every line is counts of library calls and their outcomes, so any
# change in what libreac decodes moves the file and the run exits non-zero. A
# deliberate change — a reclassification, a new field — is landed by regenerating
# the baseline in the same commit, where the diff is the report. A change that
# was not deliberate has nowhere to hide.
#
# --self-test flips a control-block byte in every frame before parsing and
# requires the output to DIFFER from the baseline. A checker that cannot fail
# reports success over any library; run this whenever a clean result is
# surprising.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CAPS=${REAC_CAPTURES:-$HOME/Devel/audio/reac-captures}
BASELINE=$ROOT/tests/corpus-baseline.txt
PER_FILE=1000000
MODE=compare

while [ $# -gt 0 ]; do
	case $1 in
	--captures) CAPS=$2; shift 2 ;;
	--baseline) BASELINE=$2; shift 2 ;;
	--per-file) PER_FILE=$2; shift 2 ;;
	--write-baseline) MODE=write; shift ;;
	--self-test) MODE=selftest; shift ;;
	*) echo "run-corpus.sh: unknown argument $1" >&2; exit 2 ;;
	esac
done

[ -d "$CAPS" ] || { echo "run-corpus.sh: no capture directory at $CAPS" >&2; exit 2; }

make -s -C "$ROOT" corpus_check

# A SEARCH THAT FINDS NOTHING LOOKS LIKE A CLEAN CORPUS. Prove the scan found
# captures before believing anything it says about them.
LIST=$(mktemp); trap 'rm -f "$LIST" "$OUT"' EXIT
find "$CAPS" -name '*.pcap*' -type f | sort > "$LIST"
N=$(wc -l < "$LIST")
[ "$N" -gt 0 ] || { echo "run-corpus.sh: no captures under $CAPS — an empty corpus proves nothing" >&2; exit 2; }
echo "run-corpus.sh: $N captures under $CAPS"

OUT=$(mktemp)
SELF=
[ "$MODE" = selftest ] && SELF=--self-test
# shellcheck disable=SC2046
xargs -a "$LIST" -d '\n' "$ROOT/corpus_check" --per-file "$PER_FILE" \
	--strip-prefix "$CAPS" $SELF > "$OUT"
sort -o "$OUT" "$OUT"

case $MODE in
write)
	cp "$OUT" "$BASELINE"
	echo "run-corpus.sh: baseline written to $BASELINE ($(wc -l < "$BASELINE") captures)"
	;;
compare)
	if diff -u "$BASELINE" "$OUT"; then
		echo "run-corpus.sh: $N captures decode exactly as the baseline records"
	else
		echo "run-corpus.sh: THE CORPUS MOVED — see the diff above." >&2
		echo "  A '-' line is what the baseline recorded, a '+' what this build does." >&2
		echo "  If the change is intended, re-run with --write-baseline in the same commit." >&2
		exit 1
	fi
	;;
selftest)
	if diff -q "$BASELINE" "$OUT" >/dev/null; then
		echo "run-corpus.sh: SELF-TEST FAILED — every frame was corrupted and the" >&2
		echo "  report did not move. This checker is inert; do not trust a green run." >&2
		exit 1
	fi
	echo "run-corpus.sh: self-test OK — a corrupted corpus goes red"
	;;
esac
