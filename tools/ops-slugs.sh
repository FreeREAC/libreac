#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# EVERY SLUG A SOURCE CITES RESOLVES IN THE OPS SIBLING. libreac's design specs, notes and
# audits live in the private FreeREAC/freereac-ops repository, under libreac/docs/; a
# source cites one by its bare slug. tools/ops-slugs.txt lists the slugs this repo owns.
# This is the ops half of tests/conformance-public-docs.sh.
#
#   tools/ops-slugs.sh              check every listed slug resolves
#   tools/ops-slugs.sh --self-test  prove the resolver against a planted ops tree
#
# THE RESOLVER, and the only code that names the ops checkout. Rungs, first hit wins:
#   $FREEREAC_OPS            an explicit checkout
#   <repo>/../freereac-ops   the sibling checkout
#   absent                   an outside clone: OPS-ABSENT, exit 77 (SKIP, nothing tested),
#                            unless FREEREAC_REQUIRE_OPS=1, which makes it a failure.
#
# Exit 0 every slug resolves, 1 one does not (or ops is required and absent), 2 NOT A
# RESULT, 77 no ops checkout.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LIST="$ROOT/tools/ops-slugs.txt"

ops_root() { # <repo root>: prints the ops checkout, or fails
	if [ -n "${FREEREAC_OPS:-}" ]; then
		[ -d "$FREEREAC_OPS" ] && { echo "$FREEREAC_OPS"; return 0; }
		echo "ops-slugs: FREEREAC_OPS=$FREEREAC_OPS is not a directory" >&2
		return 1
	fi
	[ -d "$1/../freereac-ops" ] && { (cd "$1/../freereac-ops" && pwd); return 0; }
	return 1
}

# ops_path <ops> <slug>: prints the document a slug names, or fails.
ops_path() {
	for sub in design/specs design/notes audits; do
		f="$1/libreac/docs/$sub/$2.md"
		[ -f "$f" ] && { echo "$f"; return 0; }
	done
	return 1
}

# check <repo root> <slug list>: prints a verdict line, returns 0 / 1 / 77.
check() {
	ops=$(ops_root "$1") || {
		if [ "${FREEREAC_REQUIRE_OPS:-0}" = 1 ]; then
			echo "ops-slugs: FAIL -- FREEREAC_REQUIRE_OPS=1 and no ops checkout (set FREEREAC_OPS or clone ../freereac-ops)"
			return 1
		fi
		echo "OPS-ABSENT ops-slugs: no ops checkout (FREEREAC_OPS, ../freereac-ops); nothing tested"
		return 77
	}
	n=0 missing=""
	for s in $(grep -v '^#' "$2" | grep .); do
		n=$((n + 1))
		ops_path "$ops" "$s" >/dev/null || missing="$missing $s"
	done
	if [ "$n" -eq 0 ]; then
		echo "ops-slugs: NOT A RESULT -- $2 lists no slug"
		return 2
	fi
	if [ -n "$missing" ]; then
		echo "ops-slugs: FAIL -- these slugs resolve to nothing under $ops/libreac/docs:"
		printf '  %s\n' $missing
		return 1
	fi
	echo "ops-slugs: PASS -- $n slugs resolve under $ops/libreac/docs"
	return 0
}

self_test() {
	T=$(mktemp -d)
	trap 'rm -rf "$T"' EXIT
	D=freereac-ops/libreac/docs
	mkdir -p "$T/repo/tools" "$T/$D/design/specs" "$T/$D/audits"
	printf '# slugs\n2026-01-01-a-spec\n2026-01-02-an-audit\n' > "$T/repo/tools/ops-slugs.txt"
	touch "$T/$D/design/specs/2026-01-01-a-spec.md" "$T/$D/audits/2026-01-02-an-audit.md"
	ok=1
	expect() { # <want rc> <what> -- env and args for check
		want=$1 what=$2; shift 2
		out=$(env OPS_SLUGS_LIB=1 "$@" sh -c '. "$0"; check "$1" "$1/tools/ops-slugs.txt"' "$LIB" "$T/repo" 2>&1)
		rc=$?
		if [ "$rc" = "$want" ]; then echo "  ok   $what"; else echo "  FAIL $what (rc $rc, want $want): $out"; ok=0; fi
	}
	LIB="$0"
	expect 0 "the sibling checkout resolves every slug" FREEREAC_OPS=
	mv "$T/freereac-ops" "$T/elsewhere"
	expect 77 "no sibling and no FREEREAC_OPS is OPS-ABSENT" FREEREAC_OPS=
	expect 1 "no ops checkout with FREEREAC_REQUIRE_OPS=1 fails" FREEREAC_OPS= FREEREAC_REQUIRE_OPS=1
	expect 0 "FREEREAC_OPS names the checkout" FREEREAC_OPS="$T/elsewhere"
	rm "$T/elsewhere/${D#freereac-ops/}/audits/2026-01-02-an-audit.md"
	expect 1 "a slug whose document is gone fails" FREEREAC_OPS="$T/elsewhere"
	if [ $ok = 1 ]; then echo "ops-slugs self-test: PASS"; return 0; fi
	echo "ops-slugs self-test: FAILED"; return 2
}

# Sourced by the self-test: define the functions, run nothing.
[ "${OPS_SLUGS_LIB:-0}" = 1 ] && return 0 2>/dev/null

case "${1:-}" in
	--self-test) self_test; exit $? ;;
	"") check "$ROOT" "$LIST"; exit $? ;;
	*) echo "usage: tools/ops-slugs.sh [--self-test]" >&2; exit 2 ;;
esac
