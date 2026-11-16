#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# NO BACKWARD-COMPATIBILITY MECHANISM IS KEPT. Every arm names something that existed only so
# an older program, file or caller kept working, and that is gone:
#   1  the ABI promise: LIBREAC_ABI, the layout table and its test and generator, the version
#      ranges that served it, the Python binding's any-version fallback. The soname is the
#      release version, so rpm and dpkg tie each consumer to the libreac it was built against;
#   2  the append-only enum rule in the headers;
#   3  reac_decode_plain_le, the audio decoder as it was before 0.5.0;
#   4  reac_ctrl_build_config_announce, the box named by its channel count;
#   5  the two REAC_ROLE_STATE_* aliases of REAC_CFG_ROLE_STATE_*;
#   6  the console_field name of the pace-code member;
#   7  pace-compare.sh's --prepare-userspace spelling;
#   8  the PACKET_IGNORE_OUTGOING fallback for kernels older than 4.20;
#   9  (the byte-swapped pcap is tests/test_capture.c's: it must be refused).
# Each arm is a search, so each first proves it can see a planted match. It reads files, not
# `git ls-files`, so it also runs from the source tarball.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
fail=0
say() { echo "FAIL: $*"; fail=1; }

# the sweep must reach what it speaks about
for f in include/reac/reac.h src/reac_decode.c packaging/libreac.spec debian/rules Makefile tools/pace-compare.sh; do
	[ -f "$f" ] || { echo "NOT A RESULT: $f missing, the sweep is broken, not the tree"; exit 2; }
done
printf 'x REAC_ROLE_STATE_APPLIED y\n' | grep -q 'REAC_ROLE_STATE_APPLIED' \
	|| { echo "NOT A RESULT: the search cannot see a planted match"; exit 2; }
SRCS="include src transport python tools fuzz tests packaging openwrt debian Makefile"
# in <what> <pattern>: files outside this script that match
hits() { grep -rnE --exclude=conformance-no-compat.sh -- "$1" $SRCS 2>/dev/null; }

# 1
for f in tests/test_abi_layout.c tests/abi-layout.inc tools/gen-abi-layout.py; do
	[ ! -e "$f" ] || say "row 1: $f is still here"
done
h=$(hits '^#define LIBREAC_ABI|LIBREAC_ABI \)|test_abi_layout|gen-abi-layout') && say "row 1: the ABI promise is still referenced: $(echo "$h" | head -3)"
h=$(hits 'with pkgconfig\(libreac\) < 2|libreac-dev \(<< 2') && say "row 1: a version range that served the ABI promise remains: $(echo "$h" | head -3)"
grep -q 'find_library' python/libreac/__init__.py && say "row 1: the Python binding still falls back to any libreac found by name"
# the soname is the release: it carries the version in every packaging copy
grep -q -- '-soname,libreac.so.%{version}' packaging/libreac.spec || say "row 1: the spec's soname is not the release version"
grep -q -- '-soname,libreac-transport.so.%{version}' packaging/libreac.spec || say "row 1: the spec's transport soname is not the release version"
grep -qE '^%global (abi|tabi)' packaging/libreac.spec && say "row 1: the spec still carries a hand-kept soname number"
grep -q '^TABI' debian/rules && say "row 1: debian/rules still carries a hand-kept soname number"
grep -q 'PKG_ABI' openwrt/libreac/Makefile && say "row 1: the OpenWrt recipe still reads a hand-kept soname number"

# 2
h=$(grep -rnE 'APPEND ONLY|appended, never|APPENDED|appended so every|appended rather than placed' include | grep -v '^include/reac/reac.h') \
	&& say "row 2: the append-only enum rule is still written in a header: $(echo "$h" | head -3)"

# 3
h=$(hits 'reac_decode_plain_le|PLAIN_LE_GOLDEN') && say "row 3: the pre-0.5.0 decoder is still here: $(echo "$h" | head -3)"
[ ! -e tests/test_decode_plain_le.c ] || say "row 3: tests/test_decode_plain_le.c is still here"

# 4: the box_master builder keeps its own name, which this pattern does not match
h=$(hits 'reac_ctrl_build_config_announce([^_]|$)|build_config_announce\(') && say "row 4: the width-keyed config announce is still here: $(echo "$h" | head -3)"

# 5
h=$(hits 'define REAC_ROLE_STATE_(APPLIED|REESTABLISH_PENDING)') && say "row 5: a REAC_ROLE_STATE_* alias is still defined: $(echo "$h" | head -3)"

# 6
h=$(hits 'console_field') && say "row 6: the member still has its old name: $(echo "$h" | head -3)"

# 7: the old spelling is refused as any unknown argument is
out=$(tools/pace-compare.sh --prepare-userspace true 2>&1); rc=$?
[ "$rc" -ne 0 ] && echo "$out" | grep -q 'unknown argument' || say "row 7: --prepare-userspace is still accepted"
grep -q 'PREPARE\[userspace\]' tools/pace-compare.sh && say "row 7: PREPARE[userspace] is still assigned"

# 8
grep -rn 'ifdef PACKET_IGNORE_OUTGOING' transport && say "row 8: the PACKET_IGNORE_OUTGOING fallback for old kernels is still here"

[ "$fail" = 0 ] && echo "conformance-no-compat: PASS (none of the compatibility mechanisms is back)"
exit "$fail"
