#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# DECLARE ONCE, USE EVERYWHERE (operator ruling 2026-09-25; libreac review 2026-09-25,
# M7; shared-code audit lane A). libreac owns the REAC vocabulary and the doors a binding
# reads the kernel through, and each has ONE home here:
#
#   include/reac/reac_cfg.h          the reac.cfg.* / reac.rate.* / reac.role vocabulary
#   include/reac/reac_code.h         the refusal/status token list (REAC_CODE_LIST)
#   transport/src/reac_etf_qdisc.c   the RTM_GETQDISC dump (state and counters)
#   transport/src/reac_topo.c        the topology tap read (the auxdata tag rule)
#
# A source-shape gate in the style of tools/conformance-*.sh:
#   ARM 1  every macro reac_cfg.h declares has a reader outside reac_cfg.h;
#   ARM 2  no other header restates one of its string literals under another name;
#   ARM 3  every vendored reac-pw cfg header includes <reac/reac_cfg.h>;
#   ARM 4  REAC_CODE_LIST is defined only in reac_code.h, and none of its tokens is
#          typed as a string literal anywhere else;
#   ARM 5  an RTM_GETQDISC request is built only in reac_etf_qdisc.c;
#   ARM 6  the tap's tag rule (`& TP_STATUS_VLAN_VALID`) is applied only in reac_topo.c.
# Every arm carries a planted good/bad pair, so it cannot pass (or fail) vacuously.
#
#   tests/conformance-declared-once.sh                 libreac's own tree (make test)
#   tests/conformance-declared-once.sh <dir>...        and these consumer trees: arms 2,
#                                                      4, 5 and 6 refuse a copy there too
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)

CONSUMERS=""
for d in "$@"; do
	[ -d "$d" ] || { echo "conformance-declared-once: $d is not a directory" >&2; exit 2; }
	CONSUMERS="$CONSUMERS $(cd "$d" && pwd)"
done
cd "$ROOT"

CFG=include/reac/reac_cfg.h
CODE=include/reac/reac_code.h
QDISC_HOME=transport/src/reac_etf_qdisc.c
TAP_HOME=transport/src/reac_topo.c
SCOPE="src transport include packaging/vendor tools"

# cfg_check <cfg-header> <scope dirs...>: ARMS 1-3. Prints offences, returns their count > 0.
cfg_check() {
	hdr=$1; shift
	n=0
	for m in $(sed -n 's/^#define \([A-Z_0-9]*\).*/\1/p' "$hdr" | grep -v '_H$'); do
		readers=$(grep -rlw "$m" "$@" 2>/dev/null | grep -v "^$hdr\$" | wc -l)
		if [ "$readers" -eq 0 ]; then
			echo "  ARM 1: $m is declared in $hdr and read nowhere"
			n=$((n + 1))
		fi
	done
	cfg_restated "$hdr" "$@" || n=$((n + 1))
	for v in "$@"; do
		for h in $(find "$v" -name '*_cfg.h' 2>/dev/null); do
			[ "$h" = "$hdr" ] && continue
			case "$h" in */vendor/*) ;; *) continue ;; esac
			if ! grep -q '#include <reac/reac_cfg.h>' "$h"; then
				echo "  ARM 3: $h does not include <reac/reac_cfg.h>"
				n=$((n + 1))
			fi
		done
	done
	[ "$n" -gt 0 ]
}

# cfg_restated <cfg-header> <dirs...>: ARM 2. Returns 0 when nothing is restated.
cfg_restated() {
	hdr=$1; shift
	n=0
	for m in $(sed -n 's/^#define \([A-Z_0-9]*\).*/\1/p' "$hdr" | grep -v '_H$'); do
		v=$(sed -n "s/^#define $m[[:space:]]*\(\"[^\"]*\"\).*/\1/p" "$hdr")
		[ -z "$v" ] && continue
		[ "$v" = '""' ] && continue
		# "none" is the estate's CONVENTION for "no value applies": each prop that uses
		# it declares its own idle value (REAC_BOX_MAC_NONE, ...). What this arm guards
		# is the cfg vocabulary's keys and codes being typed twice, not the convention.
		[ "$v" = '"none"' ] && continue
		dup=$(grep -rn "^#define [A-Z_0-9]*[[:space:]]*$v" "$@" 2>/dev/null \
		      | grep -v "^$hdr:" | grep -vw "$m" | head -1)
		if [ -n "$dup" ]; then
			echo "  ARM 2: $m $v is restated: $dup"
			n=$((n + 1))
		fi
	done
	[ "$n" -eq 0 ]
}

# code_once <code-header> <dirs...>: ARM 4. Returns 0 when the list has one home.
code_once() {
	hdr=$1; shift
	n=0
	for f in $(grep -rlE '^[[:space:]]*#[[:space:]]*define[[:space:]]+REAC_CODE_LIST\b' "$@" 2>/dev/null); do
		f=${f#./}
		[ "$f" = "$hdr" ] && continue
		echo "  ARM 4: $f defines a second REAC_CODE_LIST"
		n=$((n + 1))
	done
	for t in $(sed -n 's/^[[:space:]]*X([A-Z_0-9]*,[[:space:]]*"\([A-Z_0-9]*\)").*/\1/p' "$hdr"); do
		dup=$(grep -rn --include='*.c' --include='*.h' "\"$t\"" "$@" 2>/dev/null \
		      | sed 's|^\./||' | grep -v "^$hdr:" | head -1)
		if [ -n "$dup" ]; then
			echo "  ARM 4: token \"$t\" is typed outside $hdr: $dup"
			n=$((n + 1))
		fi
	done
	[ "$n" -eq 0 ]
}

# one_home <arm> <what> <regex> <home> <dirs...>: ARMS 5 and 6. Returns 0 when the
# pattern appears in C code only at <home>. A comment line that names the pattern is
# prose about the home, not a copy of it.
one_home() {
	arm=$1 what=$2 re=$3 home=$4; shift 4
	n=0
	for f in $(grep -rnE --include='*.c' --include='*.h' "$re" "$@" 2>/dev/null \
	           | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|/\*|//)' | cut -d: -f1 | sort -u); do
		f=${f#./}
		[ "$f" = "$home" ] && continue
		echo "  $arm: $f re-implements $what (its home is $home)"
		n=$((n + 1))
	done
	[ "$n" -eq 0 ]
}

QDISC_RE='=[[:space:]]*RTM_GETQDISC\b'
TAP_RE='&[[:space:]]*TP_STATUS_VLAN_VALID\b'

# The new arms, over a tree: prints offences, returns 0 when there are none.
homes_check() {
	ok=0
	code_once "$CODE" "$@" || ok=1
	one_home "ARM 5" "the RTM_GETQDISC dump" "$QDISC_RE" "$QDISC_HOME" "$@" || ok=1
	one_home "ARM 6" "the topology tap read" "$TAP_RE" "$TAP_HOME" "$@" || ok=1
	return $ok
}

# THE PLANTED PAIRS. A search that finds nothing looks like a clean tree.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
broken() {
	echo "conformance-declared-once: the detector $1 its planted $2 $3 tree${4:+ ($4)} — the arm is broken" >&2
	exit 2
}

# ARMS 1-3
mkdir -p "$tmp/cfg/good" "$tmp/cfg/bad"
printf '#define REAC_X_PROP "reac.x"\n' > "$tmp/cfg/good/decl.h"
printf '#include "decl.h"\nconst char *k = REAC_X_PROP;\n' > "$tmp/cfg/good/use.c"
printf '#define REAC_X_PROP "reac.x"\n' > "$tmp/cfg/bad/decl.h"
printf '#define REAC_PROP_X "reac.x"\n' > "$tmp/cfg/bad/copy.h"
(cd "$tmp/cfg/good" && cfg_check decl.h . >/dev/null) && broken flagged GOOD cfg
(cd "$tmp/cfg/bad" && cfg_check decl.h . >/dev/null) || broken missed BAD cfg

# ARMS 4-6: a home tree in libreac's layout, and the same tree with one copy of each.
mkdir -p "$tmp/homes/good/include/reac" "$tmp/homes/good/transport/src"
printf '#define REAC_CODE_LIST(X) \\\n\tX(RC_E_A, "E_A") \\\n\tX(RC_S_B, "S_B")\n' \
	> "$tmp/homes/good/$CODE"
printf 'void d(void) { r.type = RTM_GETQDISC; }\n' > "$tmp/homes/good/$QDISC_HOME"
printf 'int t(void) { return a.s & TP_STATUS_VLAN_VALID; }\n' > "$tmp/homes/good/$TAP_HOME"
printf '#include <reac/reac_code.h>\nint u = RC_E_A;\n' > "$tmp/homes/good/use.c"
printf '/* the home reads\n * (tp_status & TP_STATUS_VLAN_VALID), see d() = RTM_GETQDISC */\n' \
	> "$tmp/homes/good/include/reac/prose.h"
cp -R "$tmp/homes/good" "$tmp/homes/bad"
printf '#define REAC_CODE_LIST(X) X(RC_E_A, "E_A")\n' > "$tmp/homes/bad/codes.h"
printf 'void q(void) { r.type  = RTM_GETQDISC; }\n' > "$tmp/homes/bad/qdisc.c"
printf 'int v(void) { return aux.tp_status & TP_STATUS_VLAN_VALID; }\n' > "$tmp/homes/bad/main.c"
(cd "$tmp/homes/good" && homes_check . >/dev/null) || broken flagged GOOD homes
bad_out=$(cd "$tmp/homes/bad" && homes_check . 2>&1)
for arm in "ARM 4: codes.h defines" "ARM 4: token \"E_A\"" "ARM 5: qdisc.c" "ARM 6: main.c"; do
	case "$bad_out" in *"$arm"*) ;; *) broken missed BAD homes "$arm" ;; esac
done

fails=0
# shellcheck disable=SC2086
if out=$(cfg_check "$CFG" $SCOPE); then
	echo "FAIL conformance-declared-once: $CFG is not the one declaration:"
	echo "$out"
	fails=1
fi
# shellcheck disable=SC2086
if ! out=$(homes_check $SCOPE); then
	echo "FAIL conformance-declared-once: a second copy in libreac's own tree:"
	echo "$out"
	fails=1
fi
for c in $CONSUMERS; do
	out=$(cfg_restated "$CFG" "$c"; homes_check "$c")
	if [ -n "$out" ]; then
		echo "FAIL conformance-declared-once: $c carries a copy of what libreac declares:"
		echo "$out"
		fails=1
	fi
done
[ "$fails" -eq 0 ] || exit 1
echo "OK: $CFG, $CODE, the qdisc dump and the tap read each have one home, and no copy${CONSUMERS:+ (consumers:$CONSUMERS)}"
