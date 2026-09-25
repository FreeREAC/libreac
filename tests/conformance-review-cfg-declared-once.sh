#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# REVIEW 2026-09-25, finding M7 (docs/audits/2026-09-25-libreac-review.md).
#
# DECLARE ONCE, USE EVERYWHERE (operator ruling 2026-09-25). include/reac/reac_cfg.h
# says it is "ONE DECLARATION, BOTH SIDES" of the reac.cfg.* / reac.rate.* vocabulary.
# In this tree nothing reads it: the installed transport header reac_pacer.h includes
# the VENDORED reac-pw copies (packaging/vendor/reac-pw-headers/reac_rate_cfg.h,
# reac_role_cfg.h), which restate the same strings under other macro names — and
# disagree with it on the "nothing refused" answer ("none" there, "" here).
#
# A source-shape arm in the style of tools/conformance-*.sh:
#   ARM 1  every macro reac_cfg.h declares has a reader outside reac_cfg.h;
#   ARM 2  no other header restates one of its string literals under another name;
#   ARM 3  the refusal sentinel is one value on both sides.
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
		dup=$(grep -rn "^#define [A-Z_0-9]*[[:space:]]*$v" "$@" 2>/dev/null \
		      | grep -v "^$hdr:" | grep -vw "$m" | head -1)
		if [ -n "$dup" ]; then
			echo "  ARM 2: $m $v is restated: $dup"
			n=$((n + 1))
		fi
	done
	none=$(sed -n 's/^#define REAC_CFG_REFUSED_NONE[[:space:]]*\("[^"]*"\).*/\1/p' "$hdr")
	if [ -n "$none" ] && [ "$none" != '"none"' ] &&
	   grep -rq '"none" when nothing is refused' "$@" 2>/dev/null; then
		echo "  ARM 3: REAC_CFG_REFUSED_NONE is $none here; the consumer's header answers \"none\""
		n=$((n + 1))
	fi
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
	echo "conformance-review-cfg-declared-once: the detector flagged its planted GOOD tree — the arm is broken" >&2
	exit 2
fi
if ! (cd "$tmp/bad" && check decl.h . >/dev/null); then
	echo "conformance-review-cfg-declared-once: the detector missed its planted BAD tree — the arm is broken" >&2
	exit 2
fi

# shellcheck disable=SC2086
if out=$(check "$CFG" $SCOPE); then
	echo "FAIL conformance-review-cfg-declared-once: $CFG is not the one declaration:"
	echo "$out"
	exit 1
fi
echo "OK: $CFG is read, and restated nowhere"
