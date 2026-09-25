#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# DECLARE ONCE, USE EVERYWHERE (operator ruling 2026-09-25; libreac review 2026-09-25,
# M7). include/reac/reac_cfg.h is the ONE declaration of the reac.cfg.* / reac.rate.* /
# reac.role vocabulary. reac-pw's reac_rate_cfg.h and reac_role_cfg.h (snapshot in
# packaging/vendor/reac-pw-headers/) include it and alias its names; before this arm
# they spelled every string a second time and had drifted on the idle refusal value.
#
# A source-shape arm in the style of tools/conformance-*.sh:
#   ARM 1  every macro reac_cfg.h declares has a reader outside reac_cfg.h;
#   ARM 2  no other header restates one of its string literals under another name;
#   ARM 3  every vendored reac-pw cfg header includes <reac/reac_cfg.h>.
# It carries a planted good/bad pair, so it cannot pass (or fail) vacuously.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

CFG=include/reac/reac_cfg.h
SCOPE="src transport include packaging/vendor tools"

# check <cfg-header> <scope dirs...>: prints offences, returns their count > 0.
check() {
	hdr=$1; shift
	n=0
	for m in $(sed -n 's/^#define \([A-Z_0-9]*\).*/\1/p' "$hdr" | grep -v '_H$'); do
		readers=$(grep -rlw "$m" "$@" 2>/dev/null | grep -v "^$hdr\$" | wc -l)
		if [ "$readers" -eq 0 ]; then
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
			case "$h" in */vendor/*) ;; *) continue ;; esac
			if ! grep -q '#include <reac/reac_cfg.h>' "$h"; then
				echo "  ARM 3: $h does not include <reac/reac_cfg.h>"
				n=$((n + 1))
			fi
		done
	done
	[ "$n" -gt 0 ]
}

# THE PLANTED PAIR. A search that finds nothing looks like a clean tree.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/good" "$tmp/bad"
printf '#define REAC_X_PROP "reac.x"\n' > "$tmp/good/decl.h"
printf '#include "decl.h"\nconst char *k = REAC_X_PROP;\n' > "$tmp/good/use.c"
printf '#define REAC_X_PROP "reac.x"\n' > "$tmp/bad/decl.h"
printf '#define REAC_PROP_X "reac.x"\n' > "$tmp/bad/copy.h"
if (cd "$tmp/good" && check decl.h . >/dev/null); then
	echo "conformance-cfg-declared-once: the detector flagged its planted GOOD tree — the arm is broken" >&2
	exit 2
fi
if ! (cd "$tmp/bad" && check decl.h . >/dev/null); then
	echo "conformance-cfg-declared-once: the detector missed its planted BAD tree — the arm is broken" >&2
	exit 2
fi

# shellcheck disable=SC2086
if out=$(check "$CFG" $SCOPE); then
	echo "FAIL conformance-cfg-declared-once: $CFG is not the one declaration:"
	echo "$out"
	exit 1
fi
echo "OK: $CFG is read, and restated nowhere"
