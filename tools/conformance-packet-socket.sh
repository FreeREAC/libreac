#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE PROTOCOL BELONGS TO bind(), NEVER TO socket(). A packet socket created with a
# non-zero protocol registers its receive hook on EVERY interface on the host inside
# socket() itself, and stays a host-wide sniffer until the bind lands — see
# include/reac/reac_packet_socket.h for the two outages that bought this rule (#18, a
# cold-cable NIC refused a master for ever; #19, one S-1608 frame on an S-0808's wire,
# nine minutes with eight inputs off the desk).
#
# WHY THIS IS A SOURCE SCAN AND NOT ONLY A TEST. tests/test_sniffer_binds_first measures
# reac_capture_open() and tools/topo-veth-bind-probe.sh measures the tap, each on a veth
# pair in a namespace — real proof, and neither can say a word about the NEXT socket
# somebody opens. The defect is RE-INTRODUCTION: it was written correctly once (#18) and
# then written wrongly again in another file five days later. What this arm checks is that
# the vocabulary has ONE source: in the libraries, `socket(AF_PACKET` appears in the door
# and nowhere else; in the instruments under tools/ and tests/, which stay self-contained
# on purpose, the protocol argument must be 0.
#
# Exit 0 conforms, 1 a bare packet socket was found, 2 the scan could not run.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

DOOR=src/reac_packet_socket.c
[ -f "$DOOR" ] || {
	echo "conformance-packet-socket: $DOOR is missing — the door itself is gone" >&2
	exit 2
}

# Scan the CODE, not the prose: the door's own header explains the rule in the same words
# a call site would use, and a scan that cannot tell a comment from a call would report
# every one of those sentences. `cc -fpreprocessed -dD -E -P` strips comments and leaves
# everything else — no macro expansion, no includes pulled in, so nothing from another
# file can enter the result (conformance-tap-silent.sh's method).
strip_comments() { ${CC:-cc} -fpreprocessed -dD -E -P "$1" 2>/dev/null; }

# Every socket(AF_PACKET ...) call in a file, one per line, whitespace squeezed. The call
# is matched up to the statement's `;` so an argument list wrapped across lines is still
# one match.
calls_in() {
	strip_comments "$1" | tr '\n' ' ' \
		| grep -o 'socket[[:space:]]*([[:space:]]*AF_PACKET[^;]*' \
		| sed 's/[[:space:]][[:space:]]*/ /g' || true
}

# The third argument, cleaned of its trailing parenthesis and spaces.
proto_arg() { printf '%s' "$1" | cut -d, -f3 | tr -d ' )' ; }

FILES=$(find src transport/src tools tests -name '*.c' | sort)
scanned=$(printf '%s\n' "$FILES" | grep -c . || true)
if [ "$scanned" -lt 30 ]; then
	echo "conformance-packet-socket: scanned only $scanned sources — the scan is" >&2
	echo "  broken, not the tree clean." >&2
	exit 2
fi

# CONTROL 1 (presence): the door itself must show up as a call. If the extractor cannot
# find the one packet socket this tree is allowed to open, it would find none of the bad
# ones either.
door_calls=$(calls_in "$DOOR" | grep -c . || true)
if [ "$door_calls" -lt 1 ]; then
	echo "conformance-packet-socket: the extractor found no socket(AF_PACKET call in" >&2
	echo "  $DOOR, which is the one file that must have one. The scan is broken." >&2
	exit 2
fi

# CONTROL 2 (detection AND acceptance): hand the extractor one call written the way #19
# was written and one written the way the door writes it, and require EXACTLY the first to
# be caught. An absence is only a finding once the instrument has been shown to detect the
# corresponding presence — in the same run, every run. Both halves are load-bearing: a
# detector that flags everything (the extractor reading the wrong argument, measured) is
# as useless as one that flags nothing, and the "caught the bad one" half alone cannot
# tell them apart.
ctl_dir=$(mktemp -d) || exit 2
trap 'rm -rf "$ctl_dir"' EXIT INT TERM
cat > "$ctl_dir/control.c" <<'EOF'
#include <sys/socket.h>
#include <linux/if_packet.h>
#include <netinet/in.h>
int control_bad(void);
int control_good(void);
int control_bad(void)
{
	/* socket(AF_PACKET, SOCK_RAW, 0) — a comment the scan must NOT read as a call */
	return socket(AF_PACKET, SOCK_RAW, htons(0x8819));
}
int control_good(void)
{
	return socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK, 0);
}
EOF
ctl_seen=0
ctl_hits=0
ctl_calls=$(calls_in "$ctl_dir/control.c")
IFS='
'
for c in $ctl_calls; do
	[ -n "$c" ] || continue
	ctl_seen=$((ctl_seen + 1))
	[ "$(proto_arg "$c")" = "0" ] || ctl_hits=$((ctl_hits + 1))
done
unset IFS
if [ "$ctl_seen" -ne 2 ] || [ "$ctl_hits" -ne 1 ]; then
	echo "conformance-packet-socket: the detector saw $ctl_seen of the 2 calls in its own" >&2
	echo "  control file and flagged $ctl_hits of them; it must see both and flag exactly" >&2
	echo "  the one written the way #19 was written, ignoring the one in a comment and" >&2
	echo "  accepting the one with protocol 0. Nothing this run says can be believed." >&2
	exit 2
fi

fail=0

# ARM 1: in the LIBRARIES, the door is the only door. A second socket(AF_PACKET in src/ or
# transport/src/ is a second place the rule has to be remembered, which is exactly how #19
# happened after #18 was fixed.
for f in $(printf '%s\n' "$FILES" | grep -E '^(src|transport/src)/'); do
	[ "$f" = "$DOOR" ] && continue
	if [ -n "$(calls_in "$f")" ]; then
		echo "conformance-packet-socket: $f opens its own AF_PACKET socket:" >&2
		grep -n 'socket[[:space:]]*([[:space:]]*AF_PACKET' "$f" >&2 || true
		echo "  Use reac_packet_socket_bound() / _deaf() + _bind() (reac_packet_socket.h)." >&2
		fail=1
	fi
done

# ARM 2, the load-bearing one: wherever a packet socket IS opened — the door, and the
# instruments under tools/ and tests/ that stay self-contained by design — the protocol
# argument is 0. Everything else is a sniffer on every interface until it binds.
for f in $FILES; do
	IFS='
'
	for c in $(calls_in "$f"); do
		[ -n "$c" ] || continue
		p=$(proto_arg "$c")
		if [ "$p" != "0" ]; then
			unset IFS
			echo "conformance-packet-socket: $f creates a packet socket with a" >&2
			echo "  protocol: $c" >&2
			echo "  The protocol goes to bind() in sockaddr_ll.sll_protocol, together" >&2
			echo "  with the interface — the one atomic step packet(7) offers. With it" >&2
			echo "  here, the socket hears every interface on the host until the bind." >&2
			fail=1
			IFS='
'
		fi
	done
	unset IFS
done

[ "$fail" -eq 0 ] || exit 1
echo "OK: the protocol reaches bind() and never socket() — $scanned sources scanned," \
     "the libraries open packet sockets only through $DOOR, and the detector caught its" \
     "own planted call"
