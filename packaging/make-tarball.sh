#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Assemble a libreac source tarball for rpmbuild. Writes libreac-<version>.tar.gz
# to the repo root.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
# THE VERSION AND THE SONAME LIVE IN include/reac/reac.h and nowhere else. rpm
# cannot read a header, so the spec carries a copy of each and this script
# refuses to build a tarball when a copy disagrees - which is the only moment a
# copy can be caught. $1 still overrides the version for a release.
#
# THE SONAME IS CHECKED FOR THE SAME REASON THE VERSION IS, and it is the half
# that was missing when 0.6.0 removed reac_ctrl_build_name_frame/_extra_frame
# and shipped anyway as libreac.so.0: an old reac-pw loaded the new library and
# died on `undefined symbol`, because nothing compared the ABI the headers
# describe with the ABI the package builds.
HDR_V=$(awk '/^#define LIBREAC_VERSION_MAJOR/ {ma=$3}
             /^#define LIBREAC_VERSION_MINOR/ {mi=$3}
             /^#define LIBREAC_VERSION_PATCH/ {pa=$3}
             END {if (ma == "" || mi == "" || pa == "") exit 1;
                  printf "%s.%s.%s", ma, mi, pa}' "$ROOT/include/reac/reac.h")
[ -n "$HDR_V" ] || { echo "could not read LIBREAC_VERSION_* from include/reac/reac.h"; exit 1; }
SPEC_V=$(awk '/^Version:/ {print $2; exit}' "$ROOT/packaging/libreac.spec")
[ -n "$SPEC_V" ] || { echo "could not read Version: from packaging/libreac.spec"; exit 1; }
if [ "$HDR_V" != "$SPEC_V" ]; then
	echo "version drift: include/reac/reac.h says $HDR_V, packaging/libreac.spec says $SPEC_V" >&2
	echo "  The header is the definition. Bring the spec to it." >&2
	exit 1
fi

HDR_ABI=$(awk '/^#define LIBREAC_ABI[ \t]/ {print $3; exit}' "$ROOT/include/reac/reac.h")
[ -n "$HDR_ABI" ] || { echo "could not read LIBREAC_ABI from include/reac/reac.h"; exit 1; }
SPEC_ABI=$(awk '/^%global abi[ \t]/ {print $3; exit}' "$ROOT/packaging/libreac.spec")
[ -n "$SPEC_ABI" ] || { echo "could not read %global abi from packaging/libreac.spec"; exit 1; }
if [ "$HDR_ABI" != "$SPEC_ABI" ]; then
	echo "soname drift: include/reac/reac.h says LIBREAC_ABI $HDR_ABI, packaging/libreac.spec says abi $SPEC_ABI" >&2
	echo "  The header is the definition. Bring the spec to it." >&2
	echo "  A soname that lags an API break lets a stale binary load this library and die at exec." >&2
	exit 1
fi
V="${1:-$HDR_V}"
T=$(mktemp -d); D="$T/libreac-$V"; mkdir -p "$D"
rsync -a --exclude '.git' --exclude 'build' \
      "$ROOT/src" "$ROOT/include" "$ROOT/tests" "$ROOT/Makefile" "$ROOT/LICENSE" "$ROOT/README.md" "$ROOT/packaging" "$D/"
tar -czf "$ROOT/libreac-$V.tar.gz" -C "$T" "libreac-$V"
rm -rf "$T"
echo "wrote $ROOT/libreac-$V.tar.gz"
