#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# DECLARE ONCE, USE EVERYWHERE (operator ruling 2026-09-25; libreac review 2026-09-25,
# M7). include/reac/reac_cfg.h is the ONE declaration of the reac.cfg.* / reac.rate.* /
# reac.role vocabulary. reac-pw's reac_rate_cfg.h and reac_role_cfg.h include it and
# alias its names; before this arm they spelled every string a second time and had
# drifted on the idle refusal value.
#
# A source-shape arm in the style of tools/conformance-*.sh:
#   ARM 1  every macro reac_cfg.h declares has a reader outside reac_cfg.h — in this
#          tree or in reac-pw's src/ (REACPW_SRC, else the sibling ../reac-pw/src),
#          since most readers are the daemon's; with no reac-pw tree it says NOT RUN;
#   ARM 2  no other header restates one of its string literals under another name;
#   ARM 3  no other *_cfg.h exists here: the copy of reac-pw's two cfg headers this
#          repo carried until 1.6.0 (docs/design/specs/2026-09-29-shared-code-has-one-
#          home.md §2) cannot come back.
# It carries planted good/bad trees, so it cannot pass (or fail) vacuously.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

CFG=include/reac/reac_cfg.h
SCOPE="src transport include packaging tools"
REACPW_SRC=${REACPW_SRC:-$ROOT/../reac-pw/src}
[ -d "$REACPW_SRC" ] || REACPW_SRC=

# check <cfg-header> <readers-dir or ""> <scope dirs...>: prints offences, returns
# their count > 0. ARM 1 runs only when a readers dir is given.
check() {
	hdr=$1; ext=$2; shift 2
	n=0
	for m in $(sed -n 's/^#define \([A-Z_0-9]*\).*/\1/p' "$hdr" | grep -v '_H$'); do
		readers=$(grep -rlw "$m" "$@" $ext 2>/dev/null | grep -v "^$hdr\$" | wc -l)
		if [ -n "$ext" ] && [ "$readers" -eq 0 ]; then
			echo "  ARM 1: $m is declared in $hdr and read nowhere"
			n=$((n + 1))
		fi
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
	for v in "$@"; do
		for h in $(find "$v" -name '*_cfg.h' 2>/dev/null); do
			[ "$h" = "$hdr" ] && continue
			echo "  ARM 3: $h is a second cfg header; its declarations belong in $hdr"
			n=$((n + 1))
		done
	done
	[ "$n" -gt 0 ]
}

# THE PLANTED PAIR. A search that finds nothing looks like a clean tree.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/good" "$tmp/bad" "$tmp/bad3" "$tmp/unread" "$tmp/empty"
printf '#define REAC_X_PROP "reac.x"\n' > "$tmp/good/decl.h"
printf '#include "decl.h"\nconst char *k = REAC_X_PROP;\n' > "$tmp/good/use.c"
printf '#define REAC_X_PROP "reac.x"\n' > "$tmp/bad/decl.h"
printf '#define REAC_PROP_X "reac.x"\n' > "$tmp/bad/copy.h"
printf '#define REAC_X_PROP "reac.x"\n' > "$tmp/bad3/decl.h"
printf '#include "decl.h"\nconst char *k = REAC_X_PROP;\n' > "$tmp/bad3/use.c"
printf 'enum reac_x_refuse { REAC_X_NONE };\n' > "$tmp/bad3/reac_x_cfg.h"
printf '#define REAC_X_PROP "reac.x"\n' > "$tmp/unread/decl.h"
if (cd "$tmp/good" && check ./decl.h "$tmp/empty" . >/dev/null); then
	echo "conformance-cfg-declared-once: the detector flagged its planted GOOD tree — the arm is broken" >&2
	exit 2
fi
for t in bad bad3; do
	if ! (cd "$tmp/$t" && check ./decl.h "$tmp/empty" . >/dev/null); then
		echo "conformance-cfg-declared-once: the detector missed its planted $t tree — the arm is broken" >&2
		exit 2
	fi
done
if ! (cd "$tmp/unread" && check ./decl.h "$tmp/empty" . >/dev/null); then
	echo "conformance-cfg-declared-once: ARM 1 missed an unread macro — the arm is broken" >&2
	exit 2
fi

# shellcheck disable=SC2086
if out=$(check "$CFG" "$REACPW_SRC" $SCOPE); then
	echo "FAIL conformance-cfg-declared-once: $CFG is not the one declaration:"
	echo "$out"
	exit 1
fi
if [ -n "$REACPW_SRC" ]; then
	echo "OK: $CFG is read (here or in $REACPW_SRC), and restated nowhere"
else
	echo "OK: $CFG is restated nowhere; ARM 1 (every macro has a reader) NOT RUN: no reac-pw src/ beside this tree (set REACPW_SRC)"
fi
