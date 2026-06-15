#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Assemble a libreac source tarball for rpmbuild. Writes libreac-<version>.tar.gz
# to the repo root.
set -e
V="${1:-0.1.0}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d); D="$T/libreac-$V"; mkdir -p "$D"
rsync -a --exclude '.git' --exclude 'build' \
      "$ROOT/src" "$ROOT/include" "$ROOT/LICENSE" "$ROOT/README.md" "$ROOT/packaging" "$D/"
tar -czf "$ROOT/libreac-$V.tar.gz" -C "$T" "libreac-$V"
rm -rf "$T"
echo "wrote $ROOT/libreac-$V.tar.gz"
