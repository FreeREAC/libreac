#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE COMPARISON IS AN INSTRUMENT. tools/peer-replay-compare.py decides whether two replays of
# the capture corpus differ in a way the design explains; a checker that cannot be made to say
# "unexplained" would report a clean corpus over any library. This builds a one-capture corpus
# of hand-made record files (no captures, no library) and requires the checker to
#   - pass two identical replays,
#   - pass a refusal by the counter rule and by the peer binding, when the expected file names them,
#   - fail the same differences against an empty expected file,
#   - fail a difference no rule explains: an rx outcome that moved with the counter ahead, a
#     master record that changed for a frame from the bound box, a hunt verdict that moved
#     while its rival had streamed.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

python3 -I - "$T" <<'PY'
import os, struct, sys
t = sys.argv[1]
BOX, OTHER = bytes.fromhex("0040abc48041"), bytes.fromhex("0040abc66666")
def meta(i, src, ts_us):
    return struct.pack("<Q", ts_us) + src + bytes(6) + bytes(2) + struct.pack("<H", 628) + bytes(8)
def write(d, name, magic, recs):
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, name), "wb").write(magic + b"".join(recs))
def rx(out, ctr, h=0):
    return bytes([out, ctr & 255, ctr >> 8, 0]) + struct.pack("<I", h)
def ma(stb, ev, boxb, sta, acc, boxa):
    return bytes([stb, ev]) + boxb + bytes([sta, acc, 8, 8, 0]) + boxa + bytes(5)
def hu(v0, r0, mac0, obs):
    return bytes([v0, r0]) + mac0 + bytes(16) + bytes([obs]) + bytes(7)

for side in ("base", "fixed", "bad_rx", "bad_ma", "base_norival"):
    d = os.path.join(t, side, "cap")
    write(d, "meta.bin", b"PRME0001",
          [meta(i, BOX if i != 2 else OTHER, 1_000_000 + i * 250) for i in range(4)])
    ctrs = [10, 11, 9, 12]
    rxs = [rx(1, c) for c in ctrs]
    if side != "base":
        rxs[2] = rx(2, 9)
    if side == "bad_rx":
        rxs[3] = rx(2, 12)
    write(d, "rx-up.bin", b"PRRX0001", rxs)
    # master: the 3rd frame comes from OTHER while the box is bound and established
    est = 3
    recs = []
    for i in range(4):
        acc = 1
        if i == 2 and side != "base":
            acc = 0
        if i == 3 and side == "bad_ma":
            acc = 0                         # a frame from the bound box, refused
        recs.append(ma(est, 3 if i != 2 else 2, BOX, est, acc, BOX))
    write(d, "master-020000000001.bin", b"PRMA0002", recs)
    # hunt: variant 0 only. The rival 0040abc9cc03 has been seen for 4 frames: not streaming.
    rival = bytes.fromhex("0040abc9cc03")
    hrecs = []
    for i in range(4):
        v = 2 if side == "base" else 0
        if side == "base_norival":     # the base named no rival, so no rule moved the verdict
            hrecs.append(hu(v, 0, bytes(6), 1))
        else:
            hrecs.append(hu(v, 1, rival, 1))
    write(d, "hunt.bin", b"PRHU0002", hrecs)
PY
# the hunt records above carry a rival that never appears in the meta source column, so the
# evidence is "not streaming" and the base/fixed difference is explained by the hunt rule

C="python3 -I $ROOT/tools/peer-replay-compare.py"
rc=0
expect_pass() { if ! "$@" >"$T/out" 2>&1; then echo "SELFTEST FAIL: expected a pass: $*"; cat "$T/out"; rc=1; fi; }
expect_fail() { if "$@" >"$T/out" 2>&1; then echo "SELFTEST FAIL: expected red: $*"; cat "$T/out"; rc=1; fi; }

$C "$T/base" "$T/fixed" --write "$T/expected.txt" >"$T/out" 2>&1 || { cat "$T/out"; echo "SELFTEST FAIL: could not write"; exit 1; }
[ "$(grep -vc '^#' "$T/expected.txt")" = 3 ] || { echo "SELFTEST FAIL: expected three explained streams"; cat "$T/expected.txt"; rc=1; }
expect_pass $C "$T/base" "$T/fixed" --expected "$T/expected.txt"
: > "$T/empty.txt"
expect_fail $C "$T/base" "$T/fixed" --expected "$T/empty.txt"          # a new difference
expect_pass $C "$T/base" "$T/base" --expected "$T/empty.txt"           # none, none expected
expect_fail $C "$T/base" "$T/base" --expected "$T/expected.txt"        # an expected one is gone
# rx outcome moved where the counter is ahead; and only the rx file of bad_rx differs
mkdir -p "$T/only_rx/cap" && cp "$T/fixed/cap/"* "$T/only_rx/cap/" && cp "$T/bad_rx/cap/rx-up.bin" "$T/only_rx/cap/"
expect_fail $C "$T/base" "$T/only_rx" --expected "$T/expected.txt"
# a master record changed for a frame from the bound box
mkdir -p "$T/only_ma/cap" && cp "$T/fixed/cap/"* "$T/only_ma/cap/" && cp "$T/bad_ma/cap/master-020000000001.bin" "$T/only_ma/cap/"
expect_fail $C "$T/base" "$T/only_ma" --expected "$T/expected.txt"
# a hunt verdict that moved with no rival named
mkdir -p "$T/only_hu/cap" && cp "$T/base/cap/"* "$T/only_hu/cap/" && cp "$T/base_norival/cap/hunt.bin" "$T/only_hu/cap/"
expect_fail $C "$T/only_hu" "$T/fixed" --expected "$T/expected.txt"
[ $rc = 0 ] && echo "peer-replay-selftest: the comparison passes explained differences and refuses the rest"
exit $rc
