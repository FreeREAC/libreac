#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE VERSION AND THE SONAMES HAVE ONE DEFINITION EACH, AND EVERY PACKAGE COPY MUST AGREE WITH IT.
# include/reac/reac.h defines the version and libreac's soname (LIBREAC_ABI); rpm and dpkg cannot read a
# header, so packaging/libreac.spec and debian/ carry copies. A soname that lags an API break lets a
# stale binary load the new library and die at exec on an undefined symbol, so a copy that disagrees
# fails here, before a package is built:
#   ARM 1  the spec's Version and the top of debian/changelog equal the header's version;
#   ARM 2  the spec's %global abi, and the libreac<N> package of debian/control, equal LIBREAC_ABI;
#   ARM 3  the spec's %global tabi, debian/rules' TABI and the libreac-transport<N> package of
#          debian/control agree (libreac-transport's soname is its own).
# It walks files, not `git ls-files`, so it also runs from the source tarball (%check). It carries a
# planted good/bad pair, so it cannot pass (or fail) vacuously.
#
# Exit 0 conforms, 1 an arm found an offence, 2 the planted pair did not behave (NOT A RESULT).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)

# check <tree>: prints offences, returns 1 if any.
check() {
	t=$1 bad=0
	hdr=$t/include/reac/reac.h spec=$t/packaging/libreac.spec
	ver=$(awk '/^#define LIBREAC_VERSION_(MAJOR|MINOR|PATCH)/ { v = v (v == "" ? "" : ".") $3 } END { print v }' "$hdr")
	abi=$(awk '/^#define LIBREAC_ABI[ \t]/ { print $3; exit }' "$hdr")
	if [ -z "$ver" ] || [ -z "$abi" ]; then
		echo "  could not read the version or LIBREAC_ABI from include/reac/reac.h"
		return 1
	fi
	spec_ver=$(sed -n 's/^Version: *//p' "$spec")
	spec_abi=$(awk '/^%global abi[ \t]/ { print $3; exit }' "$spec")
	spec_tabi=$(awk '/^%global tabi[ \t]/ { print $3; exit }' "$spec")
	deb_ver=$(sed -n '1s/^[^ ]* (\([^)]*\)).*/\1/p' "$t/debian/changelog")
	deb_ver=${deb_ver%%~*}   # an untagged CI build stamps ~git<sha> on the top entry; the release version is before it
	deb_abi=$(sed -n 's/^Package: libreac\([0-9][0-9]*\)$/\1/p' "$t/debian/control")
	deb_tabi=$(sed -n 's/^Package: libreac-transport\([0-9][0-9]*\)$/\1/p' "$t/debian/control")
	rules_tabi=$(sed -n 's/^TABI *= *//p' "$t/debian/rules")

	[ "$spec_ver" = "$ver" ] || { echo "  ARM 1: the spec says Version $spec_ver, reac.h says $ver"; bad=1; }
	[ "$deb_ver" = "$ver" ] || { echo "  ARM 1: debian/changelog says $deb_ver, reac.h says $ver"; bad=1; }
	[ "$spec_abi" = "$abi" ] || { echo "  ARM 2: the spec says abi $spec_abi, reac.h says LIBREAC_ABI $abi"; bad=1; }
	[ "$deb_abi" = "$abi" ] || { echo "  ARM 2: debian/control names libreac$deb_abi, reac.h says LIBREAC_ABI $abi"; bad=1; }
	[ -n "$spec_tabi" ] && [ "$spec_tabi" = "$deb_tabi" ] && [ "$spec_tabi" = "$rules_tabi" ] ||
		{ echo "  ARM 3: libreac-transport's soname: spec '$spec_tabi', debian/control '$deb_tabi', debian/rules '$rules_tabi'"; bad=1; }
	return $bad
}

if ! out=$(check "$ROOT"); then
	echo "FAIL: the package copies disagree with include/reac/reac.h:"
	echo "$out"
	exit 1
fi

# The planted pair, once this tree conforms: a copy of its packaging files must conform, and a copy
# with drifted version and sonames must fail each arm.
P=$(mktemp -d)
trap 'rm -rf "$P"' EXIT
plant() { # <name>: a copy of the files the arms read
	mkdir -p "$P/$1/include/reac" "$P/$1/packaging" "$P/$1/debian"
	cp "$ROOT/include/reac/reac.h" "$P/$1/include/reac/"
	cp "$ROOT/packaging/libreac.spec" "$P/$1/packaging/"
	cp "$ROOT/debian/changelog" "$ROOT/debian/control" "$ROOT/debian/rules" "$P/$1/debian/"
}
plant good
plant bad
sed -i 's/^Version: .*/Version:        0.0.1/; s/^%global abi .*/%global abi 99/; s/^%global tabi .*/%global tabi 98/' "$P/bad/packaging/libreac.spec"
if ! check "$P/good" >/dev/null; then
	echo "NOT A RESULT: the planted conforming tree was refused"; check "$P/good"; exit 2
fi
out=$(check "$P/bad")
if [ $? -eq 0 ]; then
	echo "NOT A RESULT: the planted drifted tree was accepted"; exit 2
fi
for arm in 1 2 3; do
	echo "$out" | grep -q "ARM $arm" || { echo "NOT A RESULT: the planted tree did not trip ARM $arm"; exit 2; }
done

echo "conformance-packaging: PASS (version and sonames agree across reac.h, the spec and debian/)"
