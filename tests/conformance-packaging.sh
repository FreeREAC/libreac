#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE SONAME IS THE RELEASE VERSION, AND EVERY PACKAGE COPY MUST AGREE WITH IT.
# include/reac/reac.h defines the version. Both shared objects carry it as their soname
# (libreac.so.<version>, libreac-transport.so.<version>), so it moves with every release and
# cannot be forgotten: rpm and dpkg generate a dependency on that soname from the link, which ties
# a consumer to the libreac it was built against. rpm and dpkg cannot read a header, so
# packaging/libreac.spec and debian/ carry copies, which must say so:
#   ARM 1  the spec's Version and the top of debian/changelog equal the header's version;
#   ARM 2  the spec links libreac.so with the soname libreac.so.%{version} and keeps no hand-kept
#          soname number, and debian/control's library package is named libreac<version>, with
#          its debian/libreac<version>.install;
#   ARM 3  the same for libreac-transport: the spec's soname, debian/rules (no TABI, the version),
#          and the libreac-transport<version> package of debian/control;
#   ARM 4  debian/control builds the Python binding's conformance gate with python3.
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
	if [ -z "$ver" ]; then
		echo "  could not read the version from include/reac/reac.h"
		return 1
	fi
	spec_ver=$(sed -n 's/^Version: *//p' "$spec")
	deb_ver=$(sed -n '1s/^[^ ]* (\([^)]*\)).*/\1/p' "$t/debian/changelog")
	# A run not at a release tag stamps the top entry: build-deb.yml's dry run and staged snapshot are
	# <version>+git<time>.<sha> (above the release), its bootstrap package <version>~git<time>.<sha>
	# (below it). The release version is before either stamp.
	deb_ver=${deb_ver%%[+~]git*}
	deb_lib=$(sed -n 's/^Package: libreac\([0-9][0-9.]*\)$/\1/p' "$t/debian/control")
	deb_tlib=$(sed -n 's/^Package: libreac-transport\([0-9][0-9.]*\)$/\1/p' "$t/debian/control")
	deb_python=$(sed -n '/^Build-Depends:/p' "$t/debian/control")

	[ "$spec_ver" = "$ver" ] || { echo "  ARM 1: the spec says Version $spec_ver, reac.h says $ver"; bad=1; }
	[ "$deb_ver" = "$ver" ] || { echo "  ARM 1: debian/changelog says $deb_ver, reac.h says $ver"; bad=1; }
	grep -q -- '-soname,libreac\.so\.%{version}' "$spec" || { echo "  ARM 2: the spec's libreac soname is not libreac.so.%{version}"; bad=1; }
	grep -qE '^%global (abi|tabi)' "$spec" && { echo "  ARM 2: the spec keeps a hand-kept soname number"; bad=1; }
	[ "$deb_lib" = "$ver" ] || { echo "  ARM 2: debian/control names libreac$deb_lib, reac.h says $ver"; bad=1; }
	[ -f "$t/debian/libreac$ver.install" ] || { echo "  ARM 2: debian/libreac$ver.install is missing (the install file is named after the package)"; bad=1; }
	grep -q -- '-soname,libreac-transport\.so\.%{version}' "$spec" || { echo "  ARM 3: the spec's libreac-transport soname is not libreac-transport.so.%{version}"; bad=1; }
	grep -q '^TABI' "$t/debian/rules" && { echo "  ARM 3: debian/rules keeps a hand-kept soname number"; bad=1; }
	grep -q -- '-soname,libreac-transport\.so\.$(VERSION)' "$t/debian/rules" || { echo "  ARM 3: debian/rules' libreac-transport soname is not the version"; bad=1; }
	[ "$deb_tlib" = "$ver" ] || { echo "  ARM 3: debian/control names libreac-transport$deb_tlib, reac.h says $ver"; bad=1; }
	[ -f "$t/debian/libreac-transport$ver.install" ] || { echo "  ARM 3: debian/libreac-transport$ver.install is missing"; bad=1; }
	printf '%s\n' "$deb_python" | grep -Eq '^Build-Depends:.*(^|,)[[:space:]]*python3([[:space:]]*,|[[:space:]]*$)' ||
		{ echo "  ARM 4: debian/control must Build-Depend on python3 for tests/conformance-python-soname.sh"; bad=1; }
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
	cp "$ROOT"/debian/*.install "$P/$1/debian/"
}
plant good
plant bad
sed -i 's/^Version: .*/Version:        0.0.1/; s/-soname,libreac\.so\.%{version}/-soname,libreac.so.7/; s/-soname,libreac-transport\.so\.%{version}/-soname,libreac-transport.so.7/' "$P/bad/packaging/libreac.spec"
printf '%%global abi 7\n' >>"$P/bad/packaging/libreac.spec"
sed -i 's/^Package: libreac[0-9.]*$/Package: libreac7/; s/^Package: libreac-transport[0-9.]*$/Package: libreac-transport7/' "$P/bad/debian/control"
printf 'TABI = 7\n' >>"$P/bad/debian/rules"
sed -i 's/\(^Build-Depends:.*\)python3/\1python3-missing/' "$P/bad/debian/control"
if ! check "$P/good" >/dev/null; then
	echo "NOT A RESULT: the planted conforming tree was refused"; check "$P/good"; exit 2
fi
# The same tree as build-deb.yml stamps it outside a release tag, both ways, must conform too.
for stamp in '+git1795538786.71135ed' '~git1795538786.71135ed'; do
	plant stamped
	sed -i "1s/^\([^ ]* (\)\([^)]*\))/\1\2$stamp)/" "$P/stamped/debian/changelog"
	if ! check "$P/stamped" >/dev/null; then
		echo "NOT A RESULT: the planted tree stamped $stamp was refused"; check "$P/stamped"; exit 2
	fi
	rm -rf "$P/stamped"
done
# ...and a stamp does not hide a drifted version under it.
plant stamped
sed -i '1s/^\([^ ]* (\)[^)]*)/\10.0.1+git1795538786.71135ed)/' "$P/stamped/debian/changelog"
if check "$P/stamped" | grep -q 'debian/changelog says 0.0.1,'; then
	rm -rf "$P/stamped"
else
	echo "NOT A RESULT: a drifted version under a +git stamp was accepted"; exit 2
fi
out=$(check "$P/bad")
if [ $? -eq 0 ]; then
	echo "NOT A RESULT: the planted drifted tree was accepted"; exit 2
fi
for arm in 1 2 3 4; do
	echo "$out" | grep -q "ARM $arm" || { echo "NOT A RESULT: the planted tree did not trip ARM $arm"; exit 2; }
done

echo "conformance-packaging: PASS (the version is the soname in reac.h, the spec and debian/)"
