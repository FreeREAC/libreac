#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Assemble a libreac source tarball for rpmbuild. Writes libreac-<version>.tar.gz
# to the repo root.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
# Version single-source: the spec's Version tag (the old hardcoded 0.1.0 default
# had already drifted from the spec's 0.2.0). $1 still overrides for releases.
SPEC_V=$(awk '/^Version:/ {print $2; exit}' "$ROOT/packaging/libreac.spec")
V="${1:-$SPEC_V}"
[ -n "$V" ] || { echo "could not read Version: from packaging/libreac.spec"; exit 1; }
T=$(mktemp -d); D="$T/libreac-$V"; mkdir -p "$D"
rsync -a --exclude '.git' --exclude 'build' \
      "$ROOT/src" "$ROOT/include" "$ROOT/LICENSE" "$ROOT/README.md" "$ROOT/packaging" "$D/"
tar -czf "$ROOT/libreac-$V.tar.gz" -C "$T" "libreac-$V"
rm -rf "$T"
echo "wrote $ROOT/libreac-$V.tar.gz"
