#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# etf-veth-probe.sh -- does this kernel honour SCM_TXTIME, on a veth pair that
# touches nothing?
#
# Everything happens inside a fresh network namespace on a veth pair. It never
# names, opens or configures a real NIC, so it is safe to run beside a live rig
# and safe to run in a container. What it needs is CAP_NET_ADMIN (to make the
# namespace and the qdisc) and the sch_etf module; a container usually has
# neither, and when it does not this script SAYS WHICH ONE IS MISSING rather than
# printing a failure that reads like a kernel verdict.
#
# It runs both arms, always:
#   control   an unstamped burst, which must arrive as a burst
#   etf       the same burst with launch times, which must arrive as a grid
# A control that also arrives as a grid fails the run: an instrument that cannot
# separate the two cannot testify about either.
#
#   sudo tools/etf-veth-probe.sh [--fps 8000] [--count 200]
set -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PROBE="$ROOT/etf_probe"
NS=etfprobe$$
FPS=8000; COUNT=200

while [ $# -gt 0 ]; do
	case "$1" in
	--fps) FPS=$2; shift 2 ;;
	--count) COUNT=$2; shift 2 ;;
	-h|--help) sed -n '3,25p' "$0"; exit 0 ;;
	*) echo "etf-veth-probe: unknown argument $1" >&2; exit 2 ;;
	esac
done

say() { echo "== $*"; }
cleanup() { ip netns del "$NS" 2>/dev/null; }
trap cleanup EXIT

[ -x "$PROBE" ] || { echo "no $PROBE -- run 'make etf_probe' in $ROOT first" >&2; exit 2; }

# ---- what this environment can and cannot do, named one at a time ------------
#
# THE TOOLS FIRST, AND SEPARATELY. The first run of this script on r1 reported
# "cannot create a network namespace -- this needs CAP_NET_ADMIN" when the real
# cause was that the build container has no iproute2 at all. A diagnosis that
# names the wrong cause is worse than none: it sends the reader to fix a
# permission that was never the problem. So a missing BINARY is reported as a
# missing binary, before any capability is blamed.
missing=""
command -v ip >/dev/null || missing="$missing ip"
command -v tc >/dev/null || missing="$missing tc"
if [ -n "$missing" ]; then
	echo "REFUSED: this environment has no$missing (iproute2 is not installed)."
	echo "  That is a MISSING TOOL, not a missing capability and not a missing"
	echo "  kernel feature. Nothing about ETF has been tested either way."
	echo
	echo "  The socket-path arm still runs below, on the loopback interface."
	echo
	"$PROBE" --tx lo --rx lo --fps "$FPS" --count "$COUNT"
	rc=$?
	echo
	echo "SUMMARY: NO IPROUTE2 HERE; socket-path arm on lo exit $rc."
	exit 4
fi

if ! ip netns add "$NS" 2>/tmp/etfprobe.err; then
	echo "REFUSED: cannot create a network namespace here."
	echo "  $(cat /tmp/etfprobe.err)"
	echo "  This needs CAP_NET_ADMIN. A rootless or unprivileged container has"
	echo "  none, and that is a fact about the container, NOT about the kernel's"
	echo "  ETF support. Run it on a host, or with --privileged."
	echo
	echo "  THE SOCKET PATH IS STILL PROVABLE. Running the probe on the loopback"
	echo "  interface, which configures nothing and leaves the machine: it reports"
	echo "  whether SO_TXTIME and SCM_TXTIME are ACCEPTED here. Read that as"
	echo "  'the socket path works', never as 'ETF works' -- lo carries no etf"
	echo "  qdisc, so the launch times will be accepted and then ignored, and the"
	echo "  probe must and will say IGNORED."
	echo
	"$PROBE" --tx lo --rx lo --fps "$FPS" --count "$COUNT"
	rc=$?
	echo
	echo "SUMMARY: NO NAMESPACE HERE; socket-path arm on lo exit $rc."
	exit 4
fi
say "namespace $NS created"

ip -n "$NS" link add veth0 type veth peer name veth1 || { echo "REFUSED: veth pair"; exit 3; }
ip -n "$NS" link set veth0 up
ip -n "$NS" link set veth1 up
ip -n "$NS" link set lo up
say "veth0 <-> veth1 up inside $NS"

# sch_etf is a module on most distributions and is not loaded by default.
if ! ip netns exec "$NS" tc qdisc replace dev veth0 root etf \
	clockid CLOCK_TAI delta 300000 skip_sock_check 2>/tmp/etfprobe.err; then
	echo "REFUSED: the ETF qdisc could not be attached to a veth in this namespace."
	echo "  $(cat /tmp/etfprobe.err)"
	if ! modinfo sch_etf >/dev/null 2>&1; then
		echo "  CAUSE: this kernel has no sch_etf module at all."
	elif ! lsmod | grep -q '^sch_etf'; then
		echo "  CAUSE: sch_etf exists but is not loaded. 'modprobe sch_etf' first"
		echo "  (a container cannot load a module; the HOST must)."
	else
		echo "  CAUSE: sch_etf is loaded, so this is a permission or a veth"
		echo "  limitation, not a missing feature."
	fi
	echo
	echo "  THE SOCKET PATH IS STILL PROVABLE WITHOUT IT. Running the probe anyway:"
	echo "  it will report whether SO_TXTIME and SCM_TXTIME are ACCEPTED, which is"
	echo "  a real and separate fact from whether a qdisc honours them. Read its"
	echo "  verdict as 'the socket path works / does not work', never as 'ETF works'."
	echo
	ip netns exec "$NS" "$PROBE" --tx veth0 --rx veth1 --fps "$FPS" --count "$COUNT"
	rc=$?
	echo
	echo "SUMMARY: ETF QDISC NOT AVAILABLE HERE; socket-path arm exit $rc."
	exit 4
fi
say "etf qdisc attached to veth0: $(ip netns exec "$NS" tc qdisc show dev veth0 | head -1)"

echo
say "arm 1 of 2: the CONTROL (no launch times) — must arrive as a burst"
ip netns exec "$NS" "$PROBE" --tx veth0 --rx veth1 --fps "$FPS" --count "$COUNT" --no-etf
ctl=$?

echo
say "arm 2 of 2: ETF (launch times) — must arrive as a grid"
ip netns exec "$NS" "$PROBE" --tx veth0 --rx veth1 --fps "$FPS" --count "$COUNT"
etf=$?

echo
if [ $ctl -eq 0 ] && [ $etf -eq 0 ]; then
	echo "SUMMARY: PASS — the control separates the two and the launch times were honoured."
	exit 0
fi
echo "SUMMARY: NOT A PASS — control exit $ctl, etf exit $etf. Read the verdict lines above."
exit 1
