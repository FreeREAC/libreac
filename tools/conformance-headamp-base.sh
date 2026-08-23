#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE HEAD-AMP BASE COMES OFF THE WIRE. A box's base is the chassis strap it
# announces (config announce block[7] * 0x10), never a number derived from its
# input count. See include/reac/reac_ports.h for the firmware and corpus
# provenance.
#
# WHY THIS IS A GREP AND NOT A UNIT TEST. The risk here is RE-INTRODUCTION, not
# miscalculation. A per-width table (8 -> 0x00, 16 -> 0x20, 32 -> 0x00) agrees
# with the announce on all three chassis we own, because width and strap are
# collinear on every box in the building. So a unit test built from our own
# captures cannot tell the two derivations apart, and that is exactly how the
# table survived as long as it did. This arm does not check a value; it checks
# that the value has only one source in the code.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
fail=0

# A SEARCH THAT FINDS NOTHING LOOKS LIKE A CLEAN TREE. Prove the scan reaches
# the sources before believing anything it says about them.
scanned=$(find src include -name '*.c' -o -name '*.h' | wc -l)
if [ "$scanned" -lt 5 ]; then
	echo "conformance-headamp-base: scanned only $scanned files — the scan is broken, not the tree" >&2
	exit 2
fi

# ARM 1: the retired function's name is gone. Re-introducing the width table
# almost certainly restores the name it had.
if grep -rn 'reac_headamp_base' src include; then
	echo "conformance-headamp-base: the retired per-width base function is back." >&2
	echo "  The base is announce block[7] * 0x10, read via struct reac_box_ports." >&2
	fail=1
fi

# ARM 2, the load-bearing one: headamp_base is ASSIGNED EXACTLY ONCE in src/,
# and that assignment reads the announce. A width-derived assignment would not
# mention the block offset, so it fails here even if it is spelled differently
# from the retired function.
# `=` NOT FOLLOWED BY `=`, so an assignment is matched and a comparison is not.
# Filtering out every line containing `==` instead was wrong: it dropped
# `headamp_base = (in_ch == 16) ? ...` -- a width-derived assignment -- and
# reported it as NO assignment, which is red for the wrong reason.
assigns=$(grep -rnE '[.>]headamp_base[[:space:]]*=[^=]' src || true)
n=$(printf '%s' "$assigns" | grep -c . || true)
if [ "$n" -ne 1 ]; then
	echo "conformance-headamp-base: expected exactly 1 assignment to headamp_base in src/, found $n" >&2
	printf '%s\n' "$assigns" >&2
	echo "  One box, one place its base is decided — from the announce." >&2
	fail=1
elif ! printf '%s' "$assigns" | grep -q 'REAC_HEADAMP_BASE_OFF'; then
	echo "conformance-headamp-base: headamp_base is assigned without reading the announce:" >&2
	printf '%s\n' "$assigns" >&2
	echo "  The base is block[REAC_HEADAMP_BASE_OFF] * REAC_HEADAMP_BASE_MULTIPLIER." >&2
	echo "  Deriving it from a width or channel count is the retired defect." >&2
	fail=1
fi

[ "$fail" -eq 0 ] || exit 1
echo "OK: head-amp base has one source — the announced strap ($scanned sources scanned)"
