#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# CAN THE HARNESS TELL TWO PACERS APART? The comparison this instrument is built
# for (2026-09-13-reac-kernel-module-backend.md, lane 1) has one failure mode
# that would waste the whole comparative test day: a harness that prints a tidy
# table in which both arms look identical because it cannot resolve the
# difference. That is an absence claim from an unproven probe, and it would be
# read as "the kernel pacer is no better".
#
# So: two SYNTHETIC captures are written here, one clean 8000 fps grid and one
# with the userspace pacer's own measured miss distribution injected (the
# 250 us / 500 us / 2000 us single-debt figures from reac_pacer.h's soak), both
# pushed through the real tools/pace_hist and the real table renderer. The clean
# arm must come back silent and the jittered arm must come back worse on late
# slots, p99.9 and interval max. If the two arms read the same, the harness is
# broken and says so here rather than on the rig.
#
# It writes only into a temporary directory and opens no socket.
set -o pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

HIST=./pace_hist
[ -x "$HIST" ] || make -s pace_hist || { echo "conformance-pace-harness: cannot build pace_hist" >&2; exit 2; }

TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT

# --- two synthetic classic-pcap captures, 8000 fps, one clean, one jittered ---
python3 - "$TMP" <<'PY'
import struct, sys, os

out = sys.argv[1]
NOM_US = 125          # 8000 fps
NFRAMES = 60000       # 7.5 s of cadence: past pace_hist's 1000-frame floor
SRC = bytes.fromhex('0040abcafe01')
DST = b'\xff' * 6
FRAME = DST + SRC + b'\x88\x19' + bytes(1478)   # 1492 B, the REAC frame width

def write(path, intervals_us):
    with open(path, 'wb') as f:
        f.write(struct.pack('<IHHiIII', 0xA1B2C3D4, 2, 4, 0, 0, 262144, 1))
        t = 1_000_000_000          # an arbitrary epoch, in us
        for iv in intervals_us:
            f.write(struct.pack('<IIII', t // 1_000_000, t % 1_000_000,
                                len(FRAME), len(FRAME)))
            f.write(FRAME)
            t += iv

# The clean arm: an exact grid. Every interval is the nominal.
write(os.path.join(out, 'clean.pcap'), [NOM_US] * NFRAMES)

# The jittered arm: the SCHED_FIFO pacer's measured single-debt distribution --
# p50 250 us, p90 500 us, p95 750 us, worst 2000 us (reac_pacer.h) -- as misses
# scattered through the grid, each repaid by a short interval so the mean rate is
# unchanged. A comparison that only looked at pps would call these two identical.
iv = []
for i in range(NFRAMES):
    if i % 500 == 17:
        iv.append(NOM_US + 250); iv.append(1)
    elif i % 2000 == 33:
        iv.append(NOM_US + 500); iv.append(1)
    elif i % 10000 == 77:
        iv.append(NOM_US + 2000); iv.append(1)
    else:
        iv.append(NOM_US)
write(os.path.join(out, 'jitter.pcap'), iv)
PY
[ -s "$TMP/clean.pcap" ] && [ -s "$TMP/jitter.pcap" ] || {
	echo "conformance-pace-harness: the fixtures were not written" >&2; exit 2; }

# --- the instrument's own control runs first ---------------------------------
"$HIST" --self-test || { echo "conformance-pace-harness: pace_hist --self-test is red" >&2; exit 1; }

mkdir -p "$TMP/kmod" "$TMP/userspace"
"$HIST" --fps 8000 "$TMP/clean.pcap"  > "$TMP/kmod/wire.txt"      || exit 1
"$HIST" --fps 8000 "$TMP/jitter.pcap" > "$TMP/userspace/wire.txt" || exit 1
# The measure step's other two files, so the table renderer runs its real path.
printf 'arm=kmod\nproc_jiffies=3\nsys_busy_jiffies=210\nsys_idle_jiffies=5790\n' > "$TMP/kmod/cpu.txt"
printf 'arm=userspace\nproc_jiffies=190\nsys_busy_jiffies=260\nsys_idle_jiffies=5740\n' > "$TMP/userspace/cpu.txt"
echo '# no reac-health: line in the window' > "$TMP/kmod/health.txt"
echo 'reac-health: drift +0.0 ppm | late 3.60/s' > "$TMP/userspace/health.txt"

get() { grep -o "$2=[^ ]*" "$TMP/$1/wire.txt" | tail -1 | cut -d= -f2; }

fail=0
say() { echo "conformance-pace-harness: $*" >&2; fail=1; }

# 1. The clean arm must be SILENT. If the instrument invents late slots on an
#    exact grid, every number it prints about the kernel arm is noise.
[ "$(get kmod late1p5_ps)" = "0.0000" ] || say "clean arm reports late slots: $(get kmod late1p5_ps)/s"
[ "$(get kmod p999_us)" = "125" ]       || say "clean arm p99.9 is $(get kmod p999_us) us, want 125"
[ "$(get kmod iv_max_us)" = "125.000" ] || say "clean arm max is $(get kmod iv_max_us) us, want 125.000"

# 2. The jittered arm must be WORSE, on every measure that is supposed to
#    separate them. This is the positive control: the difference the comparative
#    test day is looking for is exactly this shape, only smaller.
awk -v a="$(get userspace late1p5_ps)" 'BEGIN{exit !(a > 1.0)}' \
	|| say "jittered arm reports only $(get userspace late1p5_ps) late/s -- not separable"
awk -v a="$(get userspace p999_us)" -v b="$(get kmod p999_us)" 'BEGIN{exit !(a > b)}' \
	|| say "jittered p99.9 ($(get userspace p999_us)) not above clean ($(get kmod p999_us))"
awk -v a="$(get userspace iv_max_us)" 'BEGIN{exit !(a >= 2125.0)}' \
	|| say "jittered max is $(get userspace iv_max_us) us -- the 2 ms miss was not seen"

# 3. And the trap the whole table exists to avoid: the two arms carry the SAME
#    packet rate. A comparison on pps alone would have called them identical.
awk -v a="$(get userspace pps)" -v b="$(get kmod pps)" \
	'BEGIN{exit !(a/b > 0.99 && a/b < 1.01)}' \
	|| say "the fixtures do not share a packet rate -- the trap is not being tested"

# 4. The table renderer runs and puts both arms in it.
TABLE=$(tools/pace-compare.sh --table "$TMP" --fps 8000 --secs 8 2>&1) || say "the table renderer failed"
echo "$TABLE" | grep -q 'userspace' || say "the table has no userspace column"
echo "$TABLE" | grep -q 'kmod'      || say "the table has no kmod column"
echo "$TABLE" | grep -q 'late >=1.5x /s' || say "the table has no late-slot row"

[ $fail -eq 0 ] || { echo "conformance-pace-harness: FAILED" >&2; exit 1; }
echo "conformance-pace-harness: the harness separates a clean grid from the pacer's own"
echo "  measured miss distribution (late/s, p99.9 and max all move; pps does not), and the"
echo "  table renders both arms."
