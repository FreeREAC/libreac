#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# m200-compare.sh -- check the desk's real M-200, on our reac-pw, against a
# mirror port, and diff what it does against what OUR master does.
#
# Rig shape this is written for (operator plan, 2026-09-17): a real Roland
# M-200 masters VLANs 11/12/13 on the boxes; enp131s0 sits on the switch's
# MIRROR port and reac-pw runs there in TAP role -- it sees the wire and
# writes nothing to it (docs/design/specs/2026-09-16-segments-and-roles-are-
# autodetected.md §3a/§A: reac-pw.conf.d/99-local.conf is a drop-in that
# LATER-WINS over reac-pw.conf, matching that spec exactly).
#
# Four jobs, each a thin driver over EXISTING tools -- this script decodes
# nothing itself (libreac + spec/reac.ksy own the protocol):
#
#   1. tap        print the override lines, then require the daemon's own
#                  roster (reac-pw/tools/roster-probe.sh) to answer tap for
#                  every segment before anything is trusted as passive.
#   2. capture     tcpdump the mirror to a pcap, filtered on the REAC
#                  ethertype spec/reac.ksy declares (0x8819) -- VLAN-tagged
#                  frames pass the same filter, the ethertype sits after the
#                  tag on those too.
#   3. analyse     run the existing analysers over the capture: corpus_check,
#                  scene-on-wire.py, recover-scene.py, pace-compare.sh
#                  (best-effort: it measures OUR pacer telemetry, so it is
#                  skipped, named, when this run has none to read).
#   4. diff        compare the M-200's master pcap against a pcap of OUR
#                  master on the same boxes, via wire_census / group_map_scan
#                  / ctrl_delta (built by `make wire-tools`) run once per
#                  side and diffed -- never a hand-rolled frame walk.
#
# Usage:
#   tools/m200-compare.sh tap [--conf-dir DIR]
#   tools/m200-compare.sh capture --iface IFACE --seconds N --out FILE.pcap
#   tools/m200-compare.sh analyse FILE.pcap [--out DIR]
#   tools/m200-compare.sh diff --m200 M200.pcap --ours OURS.pcap [--out REPORT.md]
#   tools/m200-compare.sh all --iface IFACE --seconds N --ours OURS.pcap --out DIR
#
# Taking OURS.pcap on the rig, one command (our master's own segment, not the
# mirror): sudo tcpdump -i <our-master-iface> -w ours.pcap 'ether proto 0x8819'
#
# Fixture test (proves the pipeline before rig day, no hardware needed):
#   tools/m200-compare.sh diff \
#     --m200  ~/Devel/audio/reac-captures/captures/m200i-s1608-48k-mirror__real-m200-s1608-coldboot-2026-07-11.pcap \
#     --ours  ~/Devel/audio/reac-captures/captures/m200i-s1608-48k-clean__reacpw-s1608-establish-2026-07-18.pcap \
#     --out /tmp/m200-vs-reac-pw.md
set -u
SELF=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SELF/.." && pwd)
REACPW=${REACPW_ROOT:-$HOME/Devel/audio/reac-pw}
ETHERTYPE=0x8819   # spec/reac.ksy:316-317 -- read, not guessed

die() { echo "m200-compare.sh: $*" >&2; exit 2; }

cmd=${1:-}; [ -n "$cmd" ] && shift

# --- 1. tap -----------------------------------------------------------------
do_tap() {
	local confdir="$HOME/.config/reac-pw/reac-pw.conf.d"
	while [ $# -gt 0 ]; do case $1 in
		--conf-dir) confdir=$2; shift 2 ;;
		*) die "tap: unknown argument $1" ;;
	esac; done

	cat <<'EOF'
--- the exact override (reac-pw.conf.d/99-local.conf; LATER-WINS over reac-pw.conf) ---
# ~/.config/reac-pw/reac-pw.conf.d/99-local.conf -- mirror-port rig, tap every segment
[segment enp131s0]
role = tap

[segment enp131s0.11]
role = tap

[segment enp131s0.12]
role = tap

[segment enp131s0.13]
role = tap
EOF
	echo "--- checking roster: every segment must read state=tap ---"
	command -v "$REACPW/tools/roster-probe.sh" >/dev/null 2>&1 ||
		[ -x "$REACPW/tools/roster-probe.sh" ] || die "tap: no roster-probe.sh at $REACPW/tools (set REACPW_ROOT)"
	local out; out=$("$REACPW/tools/roster-probe.sh") || { echo "$out"; die "tap: roster-probe.sh failed (exit $?)"; }
	echo "$out"
	local n; n=$(echo "$out" | grep -c '\[[0-9]*\]') || true
	[ "$n" -gt 0 ] || die "tap: roster carried no segment groups -- an empty roster proves nothing"
	local nottap; nottap=$(echo "$out" | grep '\[[0-9]*\]' | grep -v 'state=tap')
	[ -z "$nottap" ] || die "tap: segment(s) NOT reporting tap:
$nottap"
	echo "tap: all $n segment(s) confirmed tap"
}

# --- 2. capture ---------------------------------------------------------------
do_capture() {
	local iface="" seconds=60 out=""
	while [ $# -gt 0 ]; do case $1 in
		--iface) iface=$2; shift 2 ;;
		--seconds) seconds=$2; shift 2 ;;
		--out) out=$2; shift 2 ;;
		*) die "capture: unknown argument $1" ;;
	esac; done
	[ -n "$iface" ] || die "capture: --iface is required"
	[ -n "$out" ] || die "capture: --out FILE.pcap is required"
	command -v tcpdump >/dev/null 2>&1 || die "capture: no tcpdump"
	echo "capture: ${seconds}s on $iface, ether proto $ETHERTYPE (spec/reac.ksy:316-317), -> $out"
	nice -n 19 sudo tcpdump -i "$iface" -w "$out" -G "$seconds" -W 1 \
		"ether proto $ETHERTYPE" &
	local pid=$!
	wait "$pid"
	[ -s "$out" ] || die "capture: $out is empty or missing -- capture produced nothing"
	echo "capture: wrote $out ($(stat -c%s "$out") bytes)"
}

# --- 3. analyse ---------------------------------------------------------------
do_analyse() {
	local pcap="" outdir=""
	while [ $# -gt 0 ]; do case $1 in
		--out) outdir=$2; shift 2 ;;
		*) [ -z "$pcap" ] && pcap=$1 || die "analyse: unexpected argument $1"; shift ;;
	esac; done
	[ -n "$pcap" ] || die "analyse: FILE.pcap is required"
	[ -f "$pcap" ] || die "analyse: no such file $pcap"
	outdir=${outdir:-$(mktemp -d)}
	mkdir -p "$outdir"
	echo "analyse: results in $outdir"

	echo "--- corpus_check ---"
	nice -n 19 make -s -C "$ROOT" corpus_check
	"$ROOT/corpus_check" --strip-prefix "$(dirname "$pcap")" "$pcap" | tee "$outdir/corpus_check.txt"

	echo "--- scene-on-wire.py ---"
	if [ -x "$REACPW/tools/scene-on-wire.py" ]; then
		nice -n 19 python3 "$REACPW/tools/scene-on-wire.py" "$pcap" | tee "$outdir/scene-on-wire.txt"
	else
		echo "analyse: SKIPPED scene-on-wire.py -- not found under $REACPW/tools" | tee "$outdir/scene-on-wire.txt"
	fi

	echo "--- recover-scene.py ---"
	if [ -x "$REACPW/tools/recover-scene.py" ]; then
		nice -n 19 python3 "$REACPW/tools/recover-scene.py" "$pcap" | tee "$outdir/recover-scene.txt"
	else
		echo "analyse: SKIPPED recover-scene.py -- not found under $REACPW/tools" | tee "$outdir/recover-scene.txt"
	fi

	echo "--- pace-compare.sh ---"
	echo "analyse: SKIPPED pace-compare.sh -- it measures OUR pacer telemetry (thread/etf/kmod arms), not a captured M-200 master; run it separately against reac-pw's own reac-health lines if the day's job includes a pacer question" \
		| tee "$outdir/pace-compare.txt"
}

# --- 4. diff -------------------------------------------------------------------
do_diff() {
	local m200="" ours="" out="$ROOT/m200-vs-reac-pw.md"
	while [ $# -gt 0 ]; do case $1 in
		--m200) m200=$2; shift 2 ;;
		--ours) ours=$2; shift 2 ;;
		--out) out=$2; shift 2 ;;
		*) die "diff: unknown argument $1" ;;
	esac; done
	[ -n "$m200" ] && [ -f "$m200" ] || die "diff: --m200 FILE.pcap is required and must exist"
	[ -n "$ours" ] && [ -f "$ours" ] || die "diff: --ours FILE.pcap is required and must exist"

	nice -n 19 make -s -C "$ROOT" wire-tools

	local td; td=$(mktemp -d)
	local evdir="${out%.md}.evidence"
	mkdir -p "$evdir"
	for side in m200 ours; do
		local f; f=$([ "$side" = m200 ] && echo "$m200" || echo "$ours")
		"$ROOT/wire_census"     "$f" > "$td/$side.census"     2>&1
		"$ROOT/group_map_scan"  "$f" > "$td/$side.groupmap"   2>&1
		# ctrl_delta's stdout is a per-CHANGE log (thousands of lines, the full
		# evidence -- kept beside the report, never embedded); its STDERR is the
		# per-stream SUMMARY (one line per stream) -- that is what makes the
		# report one page, and it is what the table below diffs.
		"$ROOT/ctrl_delta"      "$f" > "$evdir/$side.ctrl_delta.full" 2> "$td/$side.ctrldelta"
	done

	{
		echo "# M-200 vs reac-pw -- master comparison"
		echo
		echo "M-200 capture: \`$m200\`"
		echo
		echo "reac-pw capture: \`$ours\`"
		echo
		echo "Full ctrl_delta per-frame change logs (not embedded, kept as evidence): \`$evdir/\`"
		echo
		echo "Generated by \`tools/m200-compare.sh diff\`. Each section is the unified diff of the"
		echo "SAME existing analyser (\`wire_census\`, \`group_map_scan\`, \`ctrl_delta\` -- libreac"
		echo "\`tools/\`) run once per side; \`-\` is the M-200, \`+\` is reac-pw. No line here was"
		echo "hand-decoded -- every field comes from libreac's own parser."
		echo
		echo "| section | frame-by-frame | same/different |"
		echo "|---|---|---|"
		for pair in "census:who talks, frame kinds/lengths" \
		            "groupmap:ENROLL group maps (chanmap)" \
		            "ctrldelta:control-block bytes that move (cfea cadence, identity replies)"; do
			local key=${pair%%:*} label=${pair#*:}
			if diff -q "$td/m200.$key" "$td/ours.$key" >/dev/null 2>&1; then
				echo "| $label | (identical) | same |"
			else
				echo "| $label | see \`$key.diff\` below | different |"
			fi
		done
		echo
		for key in census groupmap ctrldelta; do
			echo "## $key"
			echo '```diff'
			diff -u "$td/m200.$key" "$td/ours.$key" || true
			echo '```'
			echo
		done
	} > "$out"
	echo "diff: wrote $out"
	rm -rf "$td"
}

case "$cmd" in
	tap)     do_tap "$@" ;;
	capture) do_capture "$@" ;;
	analyse) do_analyse "$@" ;;
	diff)    do_diff "$@" ;;
	all)
		iface="" seconds=60 ours="" outdir="$ROOT/m200-compare-out"
		while [ $# -gt 0 ]; do case $1 in
			--iface) iface=$2; shift 2 ;;
			--seconds) seconds=$2; shift 2 ;;
			--ours) ours=$2; shift 2 ;;
			--out) outdir=$2; shift 2 ;;
			*) die "all: unknown argument $1" ;;
		esac; done
		mkdir -p "$outdir"
		do_tap
		do_capture --iface "$iface" --seconds "$seconds" --out "$outdir/m200-mirror.pcap"
		do_analyse "$outdir/m200-mirror.pcap" --out "$outdir/analyse"
		[ -n "$ours" ] && do_diff --m200 "$outdir/m200-mirror.pcap" --ours "$ours" --out "$outdir/m200-vs-reac-pw.md"
		;;
	*) die "usage: m200-compare.sh {tap|capture|analyse|diff|all} ..." ;;
esac
