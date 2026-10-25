#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# `make fuzz-records`: EVERY RECORD HOLDS AT THIS COMMIT. Each property check of
# fuzz/fuzz_*.c names, in its comment, one or more breakages of libreac and the seed
# that catches each one:
#
#   mutant: <path>:<line> `<old>` -> `<new>`
#   input: <seed in fuzz/corpus/<harness>/>
#
# For every record this makes the one change alone (<old> must occur exactly once on
# that line of the checked-out tree), builds the plain harness (no sanitizer, the
# driver of fuzz/records-main.c, each abort() naming its line through
# fuzz/records-abort.h), runs it on the named seed, and requires exit 134 from THAT
# check's abort(); then it puts the file back as it was. Before any change, every
# named seed must exit 0 on the unchanged harness. A record whose line moved, whose
# seed no longer reaches the check, or whose breakage another check catches first is
# red, with what happened instead.
#
# THE SABOTAGE PATCHES too: each fuzz/sabotage/<harness>.patch names above its
# `diff --git` line the check it breaks (`Breaks the check of <file>:<line>`) and the
# seed that catches it (`Caught by the seed input "<seed>"`); the patch alone must
# make that seed stop that check, the same way.
#
# THE RATCHET. A check whose comment has no record and no `not a property: <reason>`
# line is refused here, before anything is built (fuzz/checks.awk reads the comments;
# fuzz/coverage.sh refuses it too).
#
# Usage: REC_BUILD=build/records MAKE=make records.sh <harness>...
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
B=${REC_BUILD:-build/records}
MAKE=${MAKE:-make}
JOBS=${REC_JOBS:-4}
SEP=$(printf '\037')
mkdir -p "$B"

backup=
target=
pfiles=
restore() {
	if [ -n "$target" ]; then
		cp "$backup" "$target"
		target=
	fi
	for f in $pfiles; do cp "$B/patch-backup.$(echo "$f" | tr / _)" "$f"; done
	pfiles=
}
trap 'restore' EXIT
trap 'restore; exit 130' INT TERM

# The checks and the records, from the comments.
: >"$B/checks.txt"
: >"$B/records.txt"
for f in fuzz/fuzz.h $(for h in "$@"; do echo "fuzz/fuzz_$h.c"; done); do
	awk -v plain=1 -v file="$f" -f fuzz/checks.awk "$f" >>"$B/checks.txt"
	awk -v plain=1 -v records=1 -v sep="$SEP" -v file="$f" -f fuzz/checks.awk "$f" \
		>>"$B/records.txt"
done
nchk=$(awk -F: '$5 != "np"' "$B/checks.txt" | wc -l)
nnp=$(awk -F: '$5 == "np"' "$B/checks.txt" | wc -l)
undoc=$(awk -F: '$5 == "0" || $5 == "bad"' "$B/checks.txt")
if [ -n "$undoc" ] || grep -q "^BAD$SEP" "$B/records.txt"; then
	grep "^BAD$SEP" "$B/records.txt" | tr "$SEP" ' '
	echo "$undoc" | awk -F: 'NF { t = $0; sub(/^([^:]*:){5}/, "", t)
		printf "  RECORD: %s:%d (abort at line %d) names no breakage and is not marked not a property: %s\n",
		       $1, $2, $3, t }'
	echo "make fuzz-records: RED -- a check with no mutant record and no not-a-property line"
	exit 1
fi
total=$(wc -l <"$B/records.txt")

# The unchanged harnesses, and every named seed passing on them.
if ! $MAKE --no-print-directory -j"$JOBS" $(for h in "$@"; do echo "$B/fuzz_$h"; done) \
	>"$B/build.log" 2>&1; then
	cat "$B/build.log"
	echo "make fuzz-records: the unchanged harnesses do not build"
	exit 2
fi
failed=0
fail() {
	failed=$((failed + 1))
	echo "  RECORD: $1"
}
for pair in $(awk -F"$SEP" '{ h = $1; sub(/^fuzz\/fuzz_/, "", h); sub(/\.c$/, "", h)
		print h "/" $7 }' "$B/records.txt" | sort -u); do
	h=${pair%%/*}
	seed=fuzz/corpus/$pair
	if [ ! -f "$seed" ]; then
		fail "$seed: no such seed"
		continue
	fi
	timeout 120 "$B/fuzz_$h" "$seed" >/dev/null 2>"$B/run.err"
	rc=$?
	[ $rc -eq 0 ] || fail "$seed: the unchanged harness exits $rc, not 0: $(tail -1 "$B/run.err")"
done

# Each record, alone.
met=0
while IFS="$SEP" read -r file abort path line old new input; do
	h=${file#fuzz/fuzz_}
	h=${h%.c}
	where="$file:$abort: mutant $path:$line \`$old\` -> \`$new\`, input $input"
	case $path in
	/* | *..*) fail "$where: the path leaves the tree"; continue ;;
	esac
	if [ ! -f "$path" ]; then
		fail "$where: no such file"
		continue
	fi
	backup=$B/backup
	cp "$path" "$backup"
	OLD=$old NEW=$new awk -v n="$line" '
		NR == n {
			old = ENVIRON["OLD"]; rest = $0; out = ""; k = 0
			while ((i = index(rest, old)) > 0) {
				out = out substr(rest, 1, i - 1) ENVIRON["NEW"]
				rest = substr(rest, i + length(old)); k++
			}
			if (k != 1) { print k > "/dev/stderr"; exit 3 }
			$0 = out rest
		}
		{ print }
		END { if (NR < n) { print "short" > "/dev/stderr"; exit 3 } }' \
		"$backup" >"$B/mutated" 2>"$B/apply.err"
	if [ $? -ne 0 ]; then
		fail "$where: \`$old\` does not occur exactly once on that line ($(cat "$B/apply.err"))"
		continue
	fi
	target=$path
	cat "$B/mutated" >"$path"
	if ! $MAKE --no-print-directory -j"$JOBS" "$B/fuzz_$h" >"$B/mutant.log" 2>&1; then
		restore
		fail "$where: the mutant does not build: $(grep -m1 error "$B/mutant.log")"
		continue
	fi
	timeout 120 "$B/fuzz_$h" "fuzz/corpus/$h/$input" >/dev/null 2>"$B/run.err"
	rc=$?
	restore
	fired=$(sed -n 's/^records: abort at //p' "$B/run.err" | tail -1)
	if [ $rc -eq 134 ] && [ "$fired" = "$file:$abort" ]; then
		met=$((met + 1))
	elif [ $rc -eq 134 ] && [ -n "$fired" ]; then
		fail "$where: the check at $fired stops it first"
	else
		fail "$where: exit $rc${fired:+, abort at $fired}"
	fi
done <"$B/records.txt"

# Each sabotage patch, alone, the same way: above its `diff --git` line it names the
# check it breaks and the seed that catches it (OSS Verify's `mutants` directory), and
# that seed must stop THAT check, on the plain harness.
pmet=0
ptotal=0
for h in "$@"; do
	p=fuzz/sabotage/$h.patch
	[ -f "$p" ] || continue
	ptotal=$((ptotal + 1))
	at=$(sed -n 's/^Breaks the check of \(fuzz\/[^:]*:[0-9]*\) .*/\1/p' "$p")
	seed=$(sed -n 's/^Caught by the seed input "\([^"]*\)" of fuzz_.*/\1/p' "$p")
	if [ -z "$at" ] || [ -z "$seed" ]; then
		fail "$p: no \`Breaks the check of <file>:<line>\` and \`Caught by the seed input\` lines"
		continue
	fi
	abort=$(awk -F: -v f="${at%:*}" -v l="${at##*:}" '$1 == f && $2 == l { print $3 }' "$B/checks.txt")
	if [ -z "$abort" ]; then
		fail "$p: $at is not the first line of a check"
		continue
	fi
	files=$(git apply --numstat "$p" | awk '{ print $3 }')
	for f in $files; do cp "$f" "$B/patch-backup.$(echo "$f" | tr / _)"; done
	if ! git apply "$p"; then
		fail "$p: does not apply"
		continue
	fi
	pfiles=$files
	built=1
	$MAKE --no-print-directory -j"$JOBS" "$B/fuzz_$h" >"$B/mutant.log" 2>&1 || built=0
	rc=0
	[ $built -eq 1 ] && { timeout 120 "$B/fuzz_$h" "fuzz/corpus/$h/$seed" >/dev/null 2>"$B/run.err"; rc=$?; }
	restore
	fired=$(sed -n 's/^records: abort at //p' "$B/run.err" | tail -1)
	if [ $built -eq 0 ]; then
		fail "$p: the patched library does not build"
	elif [ $rc -eq 134 ] && [ "$fired" = "${at%:*}:$abort" ]; then
		pmet=$((pmet + 1))
	else
		fail "$p: seed $seed exits $rc${fired:+, abort at $fired}, not at the check of $at"
	fi
done

# Leave the plain build as the tree is, not as the last mutant was.
$MAKE --no-print-directory -j"$JOBS" $(for h in "$@"; do echo "$B/fuzz_$h"; done) \
	>"$B/build.log" 2>&1 || { cat "$B/build.log"; exit 1; }

echo "fuzz-records: $met/$total records met, on $nchk checks ($nnp more marked not a property)"
echo "fuzz-records: $pmet/$ptotal sabotage patches caught by their named seed at their named check"
if [ $failed -ne 0 ] || [ $met -ne "$total" ] || [ $pmet -ne $ptotal ]; then
	echo "make fuzz-records: RED -- $failed failures"
	exit 1
fi
echo "make fuzz-records: every record holds"
