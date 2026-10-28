#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE COVERAGE WITNESS (`make fuzz-cov`). Each harness, built for llvm-cov without
# sanitizers, runs once over its seed corpus (-runs=0 executes the corpus and nothing
# else); this prints the line coverage of the parser functions the harness exists to
# drive, and every line of them the corpus never reached. A file's other functions
# (builders, socket code) are not the harness's job and are not counted.
#
# THE FLOOR. fuzz/coverage-floor.txt holds, per harness, the number of covered parser
# lines the committed corpus reaches. A harness whose covered lines fall below its floor
# turns this red: a seed lost, a harness that stopped driving a function, or a parser
# that grew lines its corpus never reaches. Raise a floor when coverage rises; lower one
# only together with the change that removed the lines.
#
# THE HARNESS FLOOR. Every property check of the harness itself, each abort() in
# fuzz/fuzz_<harness>.c and the statement that guards it (fuzz/checks.awk), must have
# that guard executed by at least one seed (a guard with no count at all reads as not
# executed). For a `default:` arm the guard is its `switch`: that the switch ran is the
# only observable, see fuzz/README.md. A check no seed reaches is a property nobody tests,
# and the library's line count cannot see it; this names the harness file and the
# line of every such check and turns red.
#
# THE RECORD RATCHET. Every check names in its comment a breakage that it catches
# (`mutant:` and `input:`), or says in one `not a property: <reason>` line why it is
# none; a check with neither turns this red. fuzz/records.sh proves the records.
#
# Usage: coverage.sh <cov build dir> <harness>...   (LLVM_COV names llvm-cov)
set -eu
B=$1
shift
LLVM_COV=${LLVM_COV:-llvm-cov}
FLOOR=fuzz/coverage-floor.txt
[ -f "$FLOOR" ] || { echo "coverage.sh: no $FLOOR" >&2; exit 2; }
low=
dead=
norec=

# The functions each harness covers: what "every parser line the harness means to
# cover" means, one list per harness. Static functions are matched by bare name.
funcs() {
	case $1 in
	decode) echo src/reac_decode.c src/reac_upstream.c src/reac.c :: \
		reac_frame_inspect reac_decode reac_decode_plain_le reac_upstream_channels \
		reac_upstream_decode reac_frame_is_reac reac_frame_counter reac_frame_clean_len ;;
	ctrl) echo src/reac_ctrlblk.c src/reac_ctrl.c src/reac_ports.c :: \
		reac_ctrl_parse kind_of_link1 reac_ctrl_kind_name reac_ctrl_checksum_verify \
		reac_ctrl_record_cksum_verify reac_ctrl_identity_reply reac_ctrl_identity_fragment \
		reac_ctrl_headamp_record_verify reac_box_catalogue_match \
		reac_ctrl_classify_box_frame reac_ports_parse reac_ports_unknown ;;
	identity) echo src/reac_identity.c src/reac_box_facts.c :: \
		reac_identity_init reac_identity_ingest firmware_milli store_model_name u16be \
		reac_identity_fw_str reac_identity_reac_ver_str write_uint reac_box_family_of \
		reac_box_family_stem reac_box_name sb_ch sb_str sb_dec ends_with_widths \
		reac_box_catalogue_defect ;;
	disco) echo src/reac_disco.c src/reac_hunt.c :: \
		reac_disco_role_name is_box_join role_of classify_core reac_disco_classify \
		reac_disco_peer_lock_init reac_disco_classify_on_segment reac_disco_gate_init \
		reac_disco_gate_should_push reac_disco_table_init find reac_disco_table_observe \
		reac_disco_table_age reac_disco_table_json reac_hunt_verdict_name reac_hunt_init \
		reac_hunt_pin reac_hunt_silence_proven reac_hunt_observe live box_present \
		desk_geometry_live decide reac_hunt_step reac_hunt_role reac_hunt_heard_anything ;;
	scene) echo src/reac_ctrlblk.c :: \
		reac_ctrl_build_scene_step reac_ctrl_scene_set_mac reac_ctrl_scene_build \
		reac_ctrl_block_cksum_stamp ;;
	boxreg) echo src/reac_boxreg.c :: \
		reac_boxreg_init mac_is_zero reac_boxreg_find range_taken lowest_free_base \
		width_ok reac_boxreg_declare reac_boxreg_add reac_boxreg_set_name ;;
	knock) echo src/reac_knock.c :: reac_knock_init reac_knock_heard reac_knock_step ;;
	grant) echo src/reac_grant.c src/reac_headamp_tx.c :: \
		reac_grant_alloc_fits reac_grant_allocate reac_grant_headamp_value \
		reac_grant_build_sweep param_value_ok reac_headamp_tx_init reac_headamp_tx_set \
		reac_headamp_default reac_headamp_tx_effective reac_headamp_tx_arm_scene \
		reac_headamp_tx_set_resweep reac_headamp_tx_next ;;
	pcap) echo src/pcap_source.c :: rd_u32 pcap_source_open pcap_source_next \
		pcap_source_close ;;
	qdisc) echo transport/src/reac_etf_qdisc.c :: qdisc_dump qdisc_kind state_each \
		reac_etf_qdisc_state stats_take2 stats_take1 stats_each reac_etf_qdisc_stats_read ;;
	*) echo "coverage.sh: no function list for harness '$1'" >&2; exit 2 ;;
	esac
}

for h in "$@"; do
	set -- $(funcs "$h")
	files=
	while [ "$1" != "::" ]; do files="$files $1"; shift; done
	shift
	bin=$B/fuzz_$h prof=$B/$h.profdata
	echo "== fuzz_$h: $(ls fuzz/corpus/$h | wc -l) seed inputs"
	# Name Regions Miss Cover Lines Miss Cover ...: keep the listed functions.
	tot=$B/$h.covered
	"$LLVM_COV" report -show-functions "$bin" -instr-profile="$prof" $files |
	awk -v want="$*" -v tot="$tot" '
		BEGIN { n = split(want, w, " "); for (i = 1; i <= n; i++) keep[w[i]] = 1 }
		{ name = $1; sub(/^.*:/, "", name) }
		(name in keep) && NF >= 10 {
			printf "  %-34s lines %4d  missed %3d  %s\n", name, $5, $6, $7
			lines += $5; missed += $6; seen[name] = 1
		}
		END {
			for (k in keep) if (!(k in seen)) printf "  %-34s NOT FOUND in the report\n", k
			printf "  %-34s lines %4d  missed %3d  %.2f%%\n", "TOTAL", lines, missed,
			       lines ? 100 * (lines - missed) / lines : 0
			print lines - missed > tot
		}'
	for f in "$@"; do
		"$LLVM_COV" show "$bin" -instr-profile="$prof" -name-regex="^(.*:)?$f\$" $files \
			2>/dev/null |
		awk -v f="$f" -F'|' '$2 ~ /^ *0$/ {
			sub(/^ */, "", $1); printf "    unreached %s:%s %s\n", f, $1, $3 }'
	done
	src=fuzz/fuzz_$h.c
	"$LLVM_COV" show "$bin" -instr-profile="$prof" "$src" |
		awk -v file="$src" -f fuzz/checks.awk >"$B/$h.checks"
	nchk=$(wc -l <"$B/$h.checks")
	if [ "$nchk" -eq 0 ]; then
		echo "  CHECKS: no property check found in $src"
		dead="$dead $h"
	elif awk -F: '$4 == "0" || $4 == "" { n++ } END { exit !n }' "$B/$h.checks"; then
		awk -F: '$4 == "0" || $4 == "" { t = $0; sub(/^([^:]*:){5}/, "", t)
			printf "  CHECK: %s:%d no seed reaches (abort at line %d): %s\n", $1, $2, $3, t }' \
			"$B/$h.checks"
		dead="$dead $h"
	else
		echo "  checks: all $nchk property checks of $src reached"
	fi
	if awk -F: '$5 == "0" || $5 == "bad" { n++ } END { exit !n }' "$B/$h.checks"; then
		awk -F: '$5 == "0" || $5 == "bad" { t = $0; sub(/^([^:]*:){5}/, "", t)
			printf "  RECORD: %s:%d (abort at line %d) names no breakage and is not marked not a property: %s\n",
			       $1, $2, $3, t }' "$B/$h.checks"
		norec="$norec $h"
	fi
	covered=$(cat "$tot")
	floor=$(awk -v h="$h" '$1 == h { print $2 }' "$FLOOR")
	if [ -z "$floor" ]; then
		echo "  FLOOR: fuzz_$h has no line in $FLOOR"
		low="$low $h"
	elif [ "$covered" -lt "$floor" ]; then
		echo "  FLOOR: fuzz_$h covers $covered parser lines, below its floor of $floor"
		low="$low $h"
	else
		echo "  floor $floor: held ($covered covered)"
	fi
done

[ -z "$low" ] || echo "make fuzz-cov: RED -- below the coverage floor:$low"
[ -z "$dead" ] || echo "make fuzz-cov: RED -- harness checks no seed reaches:$dead"
[ -z "$norec" ] || echo "make fuzz-cov: RED -- harness checks with no mutant record:$norec"
[ -z "$low$dead$norec" ] || exit 1
echo "make fuzz-cov: every harness at or above its coverage floor, every check's guard reached and recorded"
