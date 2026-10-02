#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE DROPPED DOCUMENTS STAY DROPPED. tools/history-drop-paths.txt lists the internal
# documents libreac's public history was rewritten without (their home is freereac-ops):
#   ARM 1  none of them is in the tree again;
#   ARM 2  once the history is rewritten (tools/history-rewritten exists), no commit
#          reachable from HEAD carries one -- a branch from the old history merged back
#          would publish them again. Needs the whole history: in a shallow clone or a
#          release tarball it says why it did not run.
# It walks files for ARM 1, so it also runs from the release tarball (%check).
# It carries a planted good/bad pair, so ARM 1 cannot pass (or fail) vacuously; ARM 2 is
# tools/history-rewrite.sh's verify, which proves itself in its own --self-test.
#
# Exit 0 conforms, 1 an arm found an offence, 2 the planted pair did not behave (NOT A RESULT).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LIST="$ROOT/tools/history-drop-paths.txt"

# check <tree>: prints offences, returns 1 if any.
check() {
	bad=0
	for p in $(grep -v '^#' "$LIST" | grep .); do
		if [ -e "$1/$p" ]; then
			echo "  ARM 1: $p is back in the tree; it lives in freereac-ops/libreac/$p"
			bad=1
		fi
	done
	return $bad
}

P=$(mktemp -d)
trap 'rm -rf "$P"' EXIT
mkdir -p "$P/good/docs" "$P/bad/docs"
touch "$P/good/docs/ETF-PACING.md" "$P/bad/docs/layering.md"
if ! check "$P/good" >/dev/null; then
	echo "conformance-history-drops: NOT A RESULT -- the planted conforming tree was refused" >&2
	exit 2
fi
case $(check "$P/bad") in *"ARM 1"*) ;; *)
	echo "conformance-history-drops: NOT A RESULT -- the planted bad tree passed ARM 1" >&2
	exit 2 ;;
esac

bad=0
check "$ROOT" || bad=1
if [ ! -f "$ROOT/tools/history-rewritten" ]; then
	arm2="ARM 2 not run: the history is not rewritten yet (no tools/history-rewritten)"
elif [ "$(git -C "$ROOT" rev-parse --show-toplevel 2>/dev/null)" != "$ROOT" ]; then
	arm2="ARM 2 not run: not a git checkout (a release tarball)"
elif [ "$(git -C "$ROOT" rev-parse --is-shallow-repository)" = true ]; then
	arm2="ARM 2 not run: a shallow clone cannot see the history (fetch it whole)"
elif out=$(sh "$ROOT/tools/history-rewrite.sh" verify "$ROOT" HEAD); then
	arm2="ARM 2: no commit reachable from HEAD carries a dropped path"
else
	printf '  ARM 2: %s\n' "$out"
	bad=1
fi
if [ $bad = 0 ]; then
	echo "conformance-history-drops: PASS -- no dropped document in the tree; $arm2"
	exit 0
fi
echo "conformance-history-drops: FAIL" >&2
exit 1
