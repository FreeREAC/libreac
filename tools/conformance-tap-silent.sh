#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE TAP ROLE NEVER TRANSMITS. No announce, no join, no grant, no seglock, no
# TX socket opened at all — openmixer's master-arbitration spec, eighth
# amendment (2026-09-13), and the courtship trials of 2026-09-12 that forced it:
# a courting slave of ours kept a real desk's S-1608 from enrolling for 180 s,
# and a granted one blocked it outright.
#
# WHY THIS IS A SOURCE SCAN AND NOT A UNIT TEST. Silence is an ABSENCE, and no
# offline unit test can prove one: a test that drives reac_tap and counts zero
# outbound frames passes equally well if the transmit path is merely untaken on
# that input. The wire-level proof is reac-pw's netns arm (a fake master and a
# fake box on the far end of a veth, a packet counter on ours); this arm is the
# cheaper and stricter half — the transmit vocabulary is not REACHABLE from this
# file at all, so there is no input that could take it.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
fail=0

TAP=transport/src/reac_tap.c
CONTROL=transport/src/reac_slave.c
[ -f "$TAP" ] || { echo "conformance-tap-silent: $TAP is missing — the scan has nothing to scan" >&2; exit 2; }
[ -f "$CONTROL" ] || { echo "conformance-tap-silent: $CONTROL is missing — no positive control" >&2; exit 2; }

# The transmit vocabulary: the TX module, the two roles that drive a wire, the
# segment lock a driving role takes, and the raw send calls underneath them.
PATTERN='reac_tx_|reac_pacer_|reac_slave_|reac_seglock_|reac_ctrl_build|\bsendto\b|\bsendmsg\b|\bsend\b'

# A PROBE THAT REPORTS ABSENCE MUST FIRST BE SHOWN TO DETECT PRESENCE. Run the
# SAME expression over reac_slave.c, which transmits for a living: if it comes
# back empty the scan is broken, not the tap silent.
# Scan the CODE, not the prose: this file's own header comment names every one of
# those symbols to say it does not use them, and a scan that cannot tell a comment
# from a call would report that sentence as a transmit path. `cc -fpreprocessed -dD
# -E -P` strips comments and leaves everything else — no macro expansion, no
# includes pulled in, so nothing from another file can enter the result.
strip() { ${CC:-cc} -fpreprocessed -dD -E -P "$1" 2>/dev/null; }

control_hits=$(strip "$CONTROL" | grep -cE "$PATTERN" || true)
if [ "$control_hits" -lt 5 ]; then
	echo "conformance-tap-silent: the transmit scan found only $control_hits hits in the" >&2
	echo "  CONTROL file $CONTROL, which transmits constantly. The scan is broken." >&2
	exit 2
fi

hits=$(strip "$TAP" | grep -nE "$PATTERN" || true)
if [ -n "$hits" ]; then
	echo "conformance-tap-silent: the passive role reaches the transmit path:" >&2
	printf '%s\n' "$hits" >&2
	echo "  A tap announces nothing, joins nothing, is granted nothing and locks" >&2
	echo "  nothing. Beside a real desk that silence is what keeps the desk's own" >&2
	echo "  box able to enrol (master-arbitration, eighth amendment)." >&2
	fail=1
fi

[ "$fail" -eq 0 ] || exit 1
echo "OK: reac_tap.c reaches no transmit path ($control_hits control hits in $CONTROL prove the scan works)"
