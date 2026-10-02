#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE PUBLIC TREE CARRIES AUTHORED DOCS, NOT INTERNALS. Design specs, notes and audits live
# in the private freereac-ops sibling (libreac/docs/...); a source cites one by its bare slug
# (`2026-09-11-reac-transport-library`), never by a path, and tools/ops-slugs.sh checks that
# every slug resolves there. This arm is the public half, and needs no ops checkout:
#   ARM 1  no file names a docs/design/ or docs/audits/ path;
#   ARM 2  no file lives under docs/design/ or docs/audits/;
#   ARM 3  the README carries no build command (make, meson, cmake, ninja, rpmbuild,
#          pnpm build) -- the README is the pitch and the install; building is BUILDING.md;
#   ARM 4  BUILDING.md exists and names `make test`.
# It walks files, not `git ls-files`, so it also runs from the release tarball (%check).
# It carries a planted good/bad pair, so it cannot pass (or fail) vacuously.
#
# Exit 0 conforms, 1 an arm found an offence, 2 the planted pair did not behave (NOT A RESULT).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SELF=tests/conformance-public-docs.sh

# Built from parts so this file does not name the paths it forbids.
d=docs
PATH_RE="$d/(design|audits)/"
BUILD_RE='(^|[`[:space:]$])(make|meson|cmake|ninja|rpmbuild|pnpm build)([[:space:]`]|$)'

# check <tree>: prints offences, returns 1 if any.
check() {
	t=$1 bad=0
	hits=$(cd "$t" && grep -rIlE --exclude-dir=.git --exclude-dir=build "$PATH_RE" . 2>/dev/null \
		| sed 's#^\./##' | grep -vx "$SELF")
	if [ -n "$hits" ]; then
		echo "  ARM 1: these files name an internal docs path; cite the bare slug instead:"
		printf '    %s\n' $hits
		bad=1
	fi
	for sub in design audits; do
		if [ -d "$t/$d/$sub" ] && [ -n "$(find "$t/$d/$sub" -type f | head -1)" ]; then
			echo "  ARM 2: $d/$sub/ exists in the public tree; it belongs in freereac-ops"
			bad=1
		fi
	done
	if [ -f "$t/README.md" ] && grep -nE "$BUILD_RE" "$t/README.md" >/dev/null; then
		echo "  ARM 3: README.md carries a build command; it goes in BUILDING.md:"
		grep -nE "$BUILD_RE" "$t/README.md" | sed 's/^/    /'
		bad=1
	fi
	if ! grep -q 'make test' "$t/BUILDING.md" 2>/dev/null; then
		echo "  ARM 4: BUILDING.md is missing or does not say how to run the tests"
		bad=1
	fi
	return $bad
}

# The planted pair: a tree that conforms must pass, one with each offence must fail.
P=$(mktemp -d)
trap 'rm -rf "$P"' EXIT
mkdir -p "$P/good/src" "$P/bad/src" "$P/bad/$d/design/specs"
printf '/* see 2026-09-11-reac-transport-library */\n' > "$P/good/src/a.c"
printf '# x\n\n```\nsudo dnf install libreac-devel\n```\n' > "$P/good/README.md"
printf 'make test\n' > "$P/good/BUILDING.md"
printf '/* see %s/design/specs/2026-09-11-reac-transport-library.md */\n' "$d" > "$P/bad/src/a.c"
printf 'x\n' > "$P/bad/$d/design/specs/x.md"
printf '# x\n\n```\nmake transport\n```\n' > "$P/bad/README.md"
if ! check "$P/good" >/dev/null; then
	echo "conformance-public-docs: NOT A RESULT -- the planted conforming tree was refused" >&2
	check "$P/good" >&2
	exit 2
fi
out=$(check "$P/bad")
for arm in 'ARM 1' 'ARM 2' 'ARM 3' 'ARM 4'; do
	case $out in *"$arm"*) ;; *)
		echo "conformance-public-docs: NOT A RESULT -- the planted bad tree passed $arm" >&2
		exit 2 ;;
	esac
done

if check "$ROOT"; then
	echo "conformance-public-docs: PASS -- no internal docs path, no docs tree, README builds nothing, BUILDING.md present"
	exit 0
fi
echo "conformance-public-docs: FAIL" >&2
exit 1
