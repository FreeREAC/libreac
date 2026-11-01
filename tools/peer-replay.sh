#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# Replay the capture corpus through two builds of libreac, frame by frame, and refuse any
# difference that tests/peer-replay-expected.txt does not explain.
#
#   tools/peer-replay.sh [--base REF] [--captures DIR] [--out DIR] [--jobs N]
#   tools/peer-replay.sh --attack                 the forged-frame arm only
#   tools/peer-replay.sh --sabotage               revert each check in turn, require red
#   tools/peer-replay.sh --sabotage-replay        the same, and the whole-corpus comparison too
#   tools/peer-replay.sh --write-expected         regenerate tests/peer-replay-expected.txt
#                                                 (refused over any unexplained difference)
#
# REF defaults to origin/main. It is checked out into a scratch worktree and built there;
# this tree is the other side. Both sides build tools/peer_replay.c from THIS tree, so
# the program is the same and only the library differs.
#
# WHAT IS COMPARED (tools/peer_replay.c says how each arm is driven):
#   rx      every frame's outcome in reac_rx (decoded with the samples it wrote, not ours,
#           duplicate, bad), in each gate mode the capture calls for
#   master  every frame's effect on the master: state, accepted or not, width, declared
#           width, drop reason, box, and what the master emitted since the frame before
#   hunt    the verdict and the rival after every frame, unpinned and pinned both ways
#
# A difference is explained by tools/peer-replay-compare.py, which names the rule that
# made it from the frame itself (its source, its counter, what the binding held, how long
# the rival had streamed). Every explained difference is counted per capture and per arm
# against tests/peer-replay-expected.txt, so a NEW difference, or one that moves, is red
# even when it has a known reason. A difference no rule accounts for is red, always.
#
# THE ATTACK ARM forges frames taken from the corpus into what the findings M5,
# M6 and M8 describe and requires the base to accept them and this tree to refuse them,
# with the box's own frames as controls on both. --sabotage then puts each check back the
# way it was, one at a time, rebuilds, and requires the attack arm to go red.
#
# The captures are FreeREAC/reac-captures (git lfs pull). REAC_CAPTURES or --captures.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BASE=origin/main
CAPS=${REAC_CAPTURES:-$HOME/Devel/audio/reac-captures}
OUT=${TMPDIR:-/tmp}/peer-replay
JOBS=1
REPLAY_TOO=
MODE=replay
EXPECTED=$ROOT/tests/peer-replay-expected.txt

while [ $# -gt 0 ]; do
	case $1 in
	--base) BASE=$2; shift 2 ;;
	--captures) CAPS=$2; shift 2 ;;
	--out) OUT=$2; shift 2 ;;
	--jobs) JOBS=$2; shift 2 ;;
	--expected) EXPECTED=$2; shift 2 ;;
	--write-expected) MODE=write; shift ;;
	--attack) MODE=attack; shift ;;
	--sabotage) MODE=sabotage; shift ;;
	--sabotage-replay) MODE=sabotage; REPLAY_TOO=1; shift ;;
	*) echo "peer-replay.sh: unknown argument $1" >&2; exit 2 ;;
	esac
done

[ -d "$CAPS" ] || { echo "peer-replay.sh: no capture directory at $CAPS" >&2; exit 2; }
if find "$CAPS" -name '*.pcap' -size -1k | grep -q .; then
	echo "peer-replay.sh: $CAPS holds git-lfs pointer files, not captures: run git lfs pull" >&2
	exit 2
fi
mkdir -p "$OUT/bin"

WRAP="-Wl,--wrap=pcap_source_next -Wl,--wrap=reac_ring_write"
CFLAGS_TOOL="-O2 -std=c11 -Wall -Wextra"

# build_side NAME TREE: the library in TREE, the tool from this tree.
build_side() {
	make -s -C "$2" libreac.a libreac-transport.a >/dev/null
	${CC:-cc} $CFLAGS_TOOL -I"$2/include" "$ROOT/tools/peer_replay.c" \
	    "$2/libreac-transport.a" "$2/libreac.a" -lm -lpthread $WRAP -o "$OUT/bin/peer_replay-$1"
}

base_tree() {
	BT=$OUT/base-tree
	if [ ! -d "$BT" ] || [ "$(git -C "$BT" rev-parse HEAD 2>/dev/null)" != "$(git -C "$ROOT" rev-parse "$BASE")" ]; then
		git -C "$ROOT" worktree remove --force "$BT" 2>/dev/null || rm -rf "$BT"
		git -C "$ROOT" worktree add --detach "$BT" "$BASE" >/dev/null
	fi
	echo "$BT"
}

# The captures the attack arm forges from: a box that establishes (config-announce,
# heartbeat, JOIN, a run of returns) and a master announce, from three box families.
ATTACK_SEEDS="
captures/m200i-s0808-48k-mirror__m200-BIDIR-coldboot-2026-07-11.pcap
captures/m200i-s1608-48k-clean__reacpw-s1608-establish-2026-07-18.pcap
captures/m5000-s4000s-96k-mirror__matrix-m5000-s4000-unit1-coldconnect-2026-07-11.pcap
"

attack() {
	arc=0
	for s in $ATTACK_SEEDS; do
		"$OUT/bin/peer_replay-$1" attack "$2" "$CAPS/$s" "$OUT/forged-$1-$(basename "$s")" || arc=1
	done
	return $arc
}

# replay_side NAME: every capture through bin/peer_replay-NAME into $OUT/NAME.
replay_side() {
	rm -rf "$OUT/$1"
	mkdir -p "$OUT/$1"
	# one directory per capture, named by its path
	xargs -P "$JOBS" -I{} sh -c 'd=$(printf %s "{}" | tr / _); "$1" replay "$2/$d" "$3/{}" 2>/dev/null' \
	    _ "$OUT/bin/peer_replay-$1" "$OUT/$1" "$CAPS" < "$OUT/list.txt"
}

list_captures() {
	find "$CAPS" -name '*.pcap' | LC_ALL=C sort | sed "s|^$CAPS/||" > "$OUT/list.txt"
	echo "$(wc -l < "$OUT/list.txt") captures"
}

case $MODE in
replay|write)
	BT=$(base_tree)
	build_side base "$BT"
	build_side fixed "$ROOT"
	echo "base  $(git -C "$BT" rev-parse --short HEAD)  fixed $(git -C "$ROOT" rev-parse --short HEAD)$(git -C "$ROOT" diff --quiet HEAD -- src include transport || echo ' (+ uncommitted)')"
	list_captures
	replay_side base
	replay_side fixed
	if [ "$MODE" = write ]; then
		python3 "$ROOT/tools/peer-replay-compare.py" "$OUT/base" "$OUT/fixed" --write "$EXPECTED"
	else
		python3 "$ROOT/tools/peer-replay-compare.py" "$OUT/base" "$OUT/fixed" --expected "$EXPECTED"
	fi
	;;
attack)
	BT=$(base_tree)
	build_side base "$BT"
	build_side fixed "$ROOT"
	rc=0
	echo "== base $(git -C "$BT" rev-parse --short HEAD): the findings hold"
	attack base base || rc=1
	echo "== fixed $(git -C "$ROOT" rev-parse --short HEAD): every forged frame refused"
	attack fixed fixed || rc=1
	[ $rc = 0 ] && echo "OK: attack arm, base accepts and fixed refuses every forged frame"
	exit $rc
	;;
sabotage)
	# Each line: an id, a file, the text of one check, what it is replaced with, and no-replay
	# where the replay cannot reach the check (reac_master_rx's own gate sits behind the
	# pacer's, and the replay enters through the pacer; only the attack arm calls it direct).
	# Each line: a file, the text of one check, what it is replaced with. Applied to a
	# scratch copy of this tree, one at a time; the attack arm must go red on each.
	SAB=$OUT/sabotage
	rc=0
	if [ -n "$REPLAY_TOO" ]; then
		BT=$(base_tree)
		build_side base "$BT"
		list_captures
		replay_side base
	fi
	while IFS='|' read -r id file from to replay; do
		[ -n "$id" ] || continue
		rm -rf "$SAB"
		mkdir -p "$SAB"
		git -C "$ROOT" ls-files -z | (cd "$ROOT" && xargs -0 cp --parents -t "$SAB")
		# \t and \n in the table stand for a tab and a newline
		python3 -c '
import sys
p, a, b = sys.argv[1], sys.argv[2], sys.argv[3]
a = a.replace("\\t", "\t").replace("\\n", "\n")
b = b.replace("\\t", "\t").replace("\\n", "\n")
s = open(p).read()
if s.count(a) != 1:
    sys.exit("sabotage text not found once in %s: %r" % (p, a))
open(p, "w").write(s.replace(a, b))
' "$SAB/$file" "$from" "$to"
		build_side "sab" "$SAB" >/dev/null 2>&1
		if attack sab fixed >"$OUT/sabotage-$id.log" 2>&1; then
			echo "RED: sabotage $id left the attack arm green: that check is not proven"
			rc=1
		else
			echo "ok: sabotage $id turns the attack arm red: $(grep '<-- RED' "$OUT/sabotage-$id.log" | sed 's/^  \(M[0-9][.a-z]*\) .*/\1/;s/^  \(.*[a-z]\) \{2,\}.*/\1/' | sort -u | tr '\n' ' ')"
		fi
		# and the replay arm: the sabotaged library must no longer reproduce the
		# differences the expected file records
		if [ -n "$REPLAY_TOO" ] && [ "$replay" != no-replay ]; then
			replay_side sab
			if python3 "$ROOT/tools/peer-replay-compare.py" "$OUT/base" "$OUT/sab" --expected "$EXPECTED" >"$OUT/sabotage-$id.replay.log" 2>&1; then
				echo "RED: sabotage $id left the replay comparison green"
				rc=1
			else
				echo "ok: sabotage $id turns the replay comparison red: $(grep -c 'GONE OR MOVED\|NEW OR MOVED\|UNEXPLAINED' "$OUT/sabotage-$id.replay.log") line(s)"
			fi
		fi
	done <<'EOT'
peer-accepts|src/reac_master.c|\treturn ev == REAC_M_RX_BOX_JOIN && m->state == REAC_M_GRANTING;|\treturn 1;
pacer-gate|transport/src/reac_pacer.c|\tif (!reac_master_peer_accepts(&p->master, parsed.src, ev))\n\t\treturn;|\tif (0)\n\t\treturn;
master-rx-gate|src/reac_master.c|\tif (!reac_master_peer_accepts(m, box_src, ev))\n\t\treturn 0;|\tif (0)\n\t\treturn 0;|no-replay
rx-counter|transport/src/reac_rx.c|\t\tif (have_counter && !counter_ahead(last_counter, counter)) {|\t\tif (0) {
rx-resync-run|transport/src/reac_rx.c|#define REAC_RX_RESYNC_RUN 8u|#define REAC_RX_RESYNC_RUN 1u
hunt-rival|src/reac_hunt.c|\tif (!h->arb.have_mac)\n\t\treturn RIVAL_QUIET;|\t(void)now_ns;\n\treturn RIVAL_STREAMS;
hunt-rival-pinned|src/reac_hunt.c|ev = rival_evidence(h, now_ns);\n\t\t\tif (ev == RIVAL_STREAMS)|ev = RIVAL_STREAMS;\n\t\t\tif (ev == RIVAL_STREAMS)
hunt-rival-unpinned|src/reac_hunt.c|ev = rival_evidence(h, now_ns);\n\t\tif (ev == RIVAL_WAIT)|ev = RIVAL_STREAMS;\n\t\tif (ev == RIVAL_WAIT)
disco-frames|src/reac_disco.c|\t\tif (e->frames != UINT32_MAX)\n\t\t\te->frames++;|\t\t(void)0;
EOT
	[ $rc = 0 ] && echo "OK: every check the attack arm covers goes red when it is reverted"
	exit $rc
	;;
esac
