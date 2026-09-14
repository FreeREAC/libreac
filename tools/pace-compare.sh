#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# pace-compare.sh -- the SAME metric, on the SAME rig, for both pacer backends.
#
# The question this exists to answer is the one that decides whether the kernel
# module is worth building at all (2026-09-13-reac-kernel-module-backend.md, lane
# 1): does an hrtimer cadence measure better than the SCHED_FIFO thread? A
# comparison is only worth reading when both arms are measured by one instrument
# over one window with one definition of "late", so this script takes all three
# measures per arm and prints ONE table:
#
#   WIRE     tools/pace_hist over a tcpdump capture on the MIRROR -- external
#            truth, off the machine under test, identical for both arms.
#   PACER    the arm's own telemetry (the `reac-health:` line reac-pw writes when
#            a 10 s window closes: late wakes/s, catch-up/s, dropped/s, worst
#            single debt, transmit deficit, ring depth).
#   CPU      what the cadence costs, read TWICE on purpose -- see below.
#
# WHY CPU IS READ TWICE. The userspace pacer is a thread of reac-pw, so its cost
# is in that process's utime+stime. The kernel pacer is an hrtimer callback: its
# cost is in softirq/hardirq time and NEVER appears in any process's counters.
# Reading only the process would hand the kernel arm a free win the size of the
# whole comparison. So the system-wide busy time from /proc/stat is taken over
# the same window, and both columns are printed. Keep the box otherwise idle for
# the window or the system column measures the browser.
#
# WHAT IT REFUSES TO DO. It never restarts the daemon and never switches the
# backend: on a live console that is the operator's action, not a script's
# (feedback: rig gate is procedure, restart is operator). Given --prepare-<arm>
# it runs that command; without one it PAUSES and asks for the arm to be put in
# place, then continues. It captures, it reads, it prints.
#
#   tools/pace-compare.sh --iface mirror0 --fps 8000 --secs 60 \
#       --unit reac-pw --out /var/tmp/pace-2026-09-14
#   tools/pace-compare.sh --table /var/tmp/pace-2026-09-14   # re-print later
#
set -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
HIST="$ROOT/pace_hist"

IFACE=""; FPS=8000; SECS=60; OUT=""; UNIT="reac-pw"; SRC=""
ARMS="userspace kmod"
declare -A PREPARE
TABLE_ONLY=""

die() { echo "pace-compare: $*" >&2; exit 2; }

while [ $# -gt 0 ]; do
	case "$1" in
	--iface) IFACE=$2; shift 2 ;;
	--fps) FPS=$2; shift 2 ;;
	--secs) SECS=$2; shift 2 ;;
	--out) OUT=$2; shift 2 ;;
	--unit) UNIT=$2; shift 2 ;;
	--src) SRC=$2; shift 2 ;;
	--arms) ARMS=${2//,/ }; shift 2 ;;
	--prepare-userspace) PREPARE[userspace]=$2; shift 2 ;;
	--prepare-kmod) PREPARE[kmod]=$2; shift 2 ;;
	--table) TABLE_ONLY=$2; shift 2 ;;
	-h|--help) sed -n '3,40p' "$0"; exit 0 ;;
	*) die "unknown argument $1" ;;
	esac
done

# ---- the instrument is checked BEFORE it is believed --------------------------
# pace_hist's own control: a perfect grid must be silent and three injected
# stalls must all be found. An instrument that has not been shown to detect the
# presence cannot testify about the absence, and "the kernel arm had no late
# slots" is an absence claim.
[ -x "$HIST" ] || die "no $HIST -- run 'make pace_hist' in $ROOT first"
"$HIST" --self-test || die "pace_hist --self-test FAILED; its verdicts mean nothing"

# ---------------------------------------------------------------------------
# One arm's measurement: a directory holding wire.pcap, health.txt, cpu.txt.
# ---------------------------------------------------------------------------
measure_arm() {
	local arm=$1 dir="$OUT/$arm"
	mkdir -p "$dir" || die "cannot create $dir"

	if [ -n "${PREPARE[$arm]}" ]; then
		echo "== $arm: preparing: ${PREPARE[$arm]}"
		eval "${PREPARE[$arm]}" || die "prepare for $arm failed"
	else
		echo
		echo "== $arm: put the rig on the '$arm' backend now."
		echo "   (this script does not restart the daemon; the operator does)"
		read -r -p "   press RETURN when the $arm arm is established and passing audio: " _
	fi

	# The daemon's pid, for the process CPU column. Absent in an arm where the
	# cadence is entirely in the kernel is FINE and is printed as such -- it is
	# not an error and must not read as zero cost.
	local pid
	pid=$(systemctl show -p MainPID --value "$UNIT" 2>/dev/null)
	[ "$pid" = "0" ] && pid=""
	[ -z "$pid" ] && pid=$(pgrep -x reac-pw | head -1)

	local start_epoch
	start_epoch=$(date -u +%Y-%m-%d' '%H:%M:%S)

	# CPU, before.
	read -r _ u n s i rest < /proc/stat
	local sys_busy_0=$(( u + n + s ))
	local sys_idle_0=$i
	local proc_0=""
	[ -n "$pid" ] && proc_0=$(awk '{print $14+$15}' "/proc/$pid/stat" 2>/dev/null)

	echo "== $arm: capturing $SECS s on $IFACE"
	# -p: do NOT put the interface into promiscuous mode if it is already a
	# mirror/monitor port fed everything -- a capture must not change the
	# segment it is measuring. Classic pcap (no -j): pace_hist reads
	# microseconds and refuses a nanosecond file by name.
	timeout $((SECS + 15)) tcpdump -i "$IFACE" -p -s 128 -w "$dir/wire.pcap" \
		-G "$SECS" -W 1 'ether proto 0x8819' >"$dir/tcpdump.log" 2>&1
	local trc=$?

	# CPU, after.
	read -r _ u n s i rest < /proc/stat
	local sys_busy_1=$(( u + n + s ))
	local sys_idle_1=$i
	local proc_1=""
	[ -n "$pid" ] && proc_1=$(awk '{print $14+$15}' "/proc/$pid/stat" 2>/dev/null)

	{
		echo "arm=$arm"
		echo "tcpdump_rc=$trc"
		echo "pid=${pid:-none}"
		echo "sys_busy_jiffies=$(( sys_busy_1 - sys_busy_0 ))"
		echo "sys_idle_jiffies=$(( sys_idle_1 - sys_idle_0 ))"
		if [ -n "$proc_0" ] && [ -n "$proc_1" ]; then
			echo "proc_jiffies=$(( proc_1 - proc_0 ))"
		else
			echo "proc_jiffies=n/a"
		fi
	} > "$dir/cpu.txt"

	# The arm's own telemetry over the same window. Absent is reported as
	# absent: an arm whose pacer publishes nothing has NOT scored zero.
	journalctl -u "$UNIT" --since "$start_epoch" --no-pager 2>/dev/null \
		| grep -F 'reac-health:' > "$dir/health.txt"
	[ -s "$dir/health.txt" ] || echo "# no reac-health: line in the window" > "$dir/health.txt"

	local args=(--fps "$FPS")
	[ -n "$SRC" ] && args+=(--src "$SRC")
	"$HIST" "${args[@]}" "$dir/wire.pcap" > "$dir/wire.txt" 2>"$dir/wire.err"
	echo "== $arm: done -> $dir"
}

# ---------------------------------------------------------------------------
# The table. Rows are metrics, columns are arms, so the eye compares across.
# ---------------------------------------------------------------------------
field() {   # field <file> <key>   -- last occurrence, or a dash
	local v
	v=$(grep -o "$2=[^ ]*" "$1" 2>/dev/null | tail -1 | cut -d= -f2)
	echo "${v:--}"
}

print_table() {
	local base=$1 arm dir
	local -a cols=()
	for arm in $ARMS; do [ -d "$base/$arm" ] && cols+=("$arm"); done
	[ ${#cols[@]} -gt 0 ] || die "no arm directories under $base -- nothing was measured"

	printf '\n%s\n' "pacer comparison -- $base, ${FPS} fps, ${SECS} s window"
	printf '%-26s' "metric"; for arm in "${cols[@]}"; do printf '%18s' "$arm"; done; echo
	printf '%-26s' "--------------------------"; for arm in "${cols[@]}"; do printf '%18s' "-----------------"; done; echo

	row_wire() {   # row_wire <label> <key>
		printf '%-26s' "$1"
		for arm in "${cols[@]}"; do printf '%18s' "$(field "$base/$arm/wire.txt" "$2")"; done
		echo
	}
	echo "WIRE (mirror capture, one instrument, both arms)"
	row_wire "  frames"            frames
	row_wire "  span (s)"          span_s
	row_wire "  packet rate (pps)" pps
	row_wire "  rate snapped (Hz)" rate_hz
	row_wire "  interval mean (us)" iv_mean_us
	row_wire "  interval sd (us)"  iv_sd_us
	row_wire "  interval max (us)" iv_max_us
	row_wire "  p50 (us)"          p50_us
	row_wire "  p99 (us)"          p99_us
	row_wire "  p99.9 (us)"        p999_us
	row_wire "  late >=1.5x /s"    late1p5_ps
	row_wire "  late >=4x /s"      late4x_ps
	row_wire "  catch-up /s"       catchup_ps

	echo "PACER (the arm's own telemetry -- a self-report, by construction)"
	for arm in "${cols[@]}"; do
		printf '    %-10s %s\n' "$arm:" "$(tail -1 "$base/$arm/health.txt" 2>/dev/null | sed 's/.*reac-health: //')"
	done

	echo "CPU over the window (100 jiffies = 1 s of one core on this kernel)"
	printf '%-26s' "  daemon process (jiffies)"
	for arm in "${cols[@]}"; do printf '%18s' "$(field "$base/$arm/cpu.txt" proc_jiffies)"; done
	echo
	printf '%-26s' "  system busy (jiffies)"
	for arm in "${cols[@]}"; do printf '%18s' "$(field "$base/$arm/cpu.txt" sys_busy_jiffies)"; done
	echo
	printf '%-26s' "  system idle (jiffies)"
	for arm in "${cols[@]}"; do printf '%18s' "$(field "$base/$arm/cpu.txt" sys_idle_jiffies)"; done
	echo
	cat <<'NOTE'

  The kernel arm's cadence runs in an hrtimer callback and appears in NO
  process's counters: read its cost in the system row, never the process row.
  A process row that fell to nearly nothing while the system row held is work
  that moved, not work that vanished.

  Resolution: 1 us (classic pcap). A difference smaller than a microsecond is
  below this instrument and must not be claimed from this table.
NOTE
}

if [ -n "$TABLE_ONLY" ]; then
	OUT=$TABLE_ONLY
	print_table "$OUT"
	exit 0
fi

[ -n "$IFACE" ] || die "--iface <mirror interface> is required"
[ -n "$OUT" ] || die "--out <directory> is required"
command -v tcpdump >/dev/null || die "tcpdump not found"
mkdir -p "$OUT" || die "cannot create $OUT"

for arm in $ARMS; do measure_arm "$arm"; done
print_table "$OUT"
