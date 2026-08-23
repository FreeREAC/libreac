#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Assemble a libreac source tarball for rpmbuild. Writes libreac-<version>.tar.gz
# to the repo root.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
# THE VERSION LIVES IN include/reac/reac.h and nowhere else. rpm cannot read a
# header, so the spec carries a copy of it and this script refuses to build a
# tarball when the two disagree - which is the only moment the copy can be
# caught. $1 still overrides for a release.
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
V="${1:-$HDR_V}"
T=$(mktemp -d); D="$T/libreac-$V"; mkdir -p "$D"
rsync -a --exclude '.git' --exclude 'build' \
      "$ROOT/src" "$ROOT/include" "$ROOT/tests" "$ROOT/Makefile" "$ROOT/LICENSE" "$ROOT/README.md" "$ROOT/packaging" "$D/"
tar -czf "$ROOT/libreac-$V.tar.gz" -C "$T" "libreac-$V"
rm -rf "$T"
echo "wrote $ROOT/libreac-$V.tar.gz"
