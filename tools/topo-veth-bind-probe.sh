#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# topo-veth-bind-probe.sh — the netns arm for libreac #18: the topology tap must
# never hear a frame from an interface it was not bound to.
#
# Two veth pairs inside a fresh USER+NET namespace: `omxA/omxAp` is the link the
# tap binds to, `omxB/omxBp` is the other link a real box would be on. Nothing
# real is named, opened or configured, so this is safe beside the live rig — and
# `unshare -Ur` means it needs no root at all: CAP_NET_ADMIN and CAP_NET_RAW are
# held only inside the namespace it makes.
#
# It reports a MISSING TOOL as a missing tool and an UNAVAILABLE namespace as
# that, never as a verdict about the library (etf-veth-probe.sh's lesson).
# Exit 0 pass, 1 the tap leaked, 2 nothing was measured.
#
#   tools/topo-veth-bind-probe.sh [opens]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PROBE="$ROOT/topo_bind_probe"
OPENS=${1:-400}

if [ "$REAC_TOPO_PROBE_INNER" != "1" ]; then
	[ -x "$PROBE" ] || {
		echo "topo-veth-bind-probe: no $PROBE — run 'make topo_bind_probe' first" >&2
		exit 2
	}
	command -v ip >/dev/null || {
		echo "REFUSED: no iproute2 (\`ip\`) in this environment. That is a MISSING" >&2
		echo "  TOOL, not a verdict about the tap: nothing has been tested." >&2
		exit 2
	}
	unshare -Ur -n true 2>/dev/null || {
		echo "REFUSED: this environment cannot make a user+net namespace (a" >&2
		echo "  container without CAP_SYS_ADMIN, or userns disabled). Nothing has" >&2
		echo "  been tested — run it on a host shell." >&2
		exit 2
	}
	REAC_TOPO_PROBE_INNER=1 exec unshare -Ur -n "$0" "$OPENS"
fi

ip link add name omxA type veth peer name omxAp
ip link add name omxB type veth peer name omxBp
for i in omxA omxAp omxB omxBp; do ip link set "$i" up; done
# The namespace dies with this process, and the veths with it — no cleanup path
# that could outlive a crash and leave interfaces behind.

exec "$PROBE" omxA omxAp omxBp omxB "$OPENS"
