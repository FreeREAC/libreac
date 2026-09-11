#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build this package's RPMs.
#
# _topdir is forced to the PHYSICAL path of ~/rpmbuild. Where ~/rpmbuild is a
# symlink, meson/ninja record the resolved physical directory in DW_AT_comp_dir
# while rpm's debugedit looks under the logical one, so it finds no sources and
# the build dies at the very end with:
#
#     error: Empty %files file .../debugsourcefiles.list
#
# after %build and %check have both passed -- which reads like a packaging bug
# and is not one.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
# TWO SPECS SINCE 0.9.0 (libreac.spec, libreac-transport.spec), both built from the SAME
# tarball -- `ls | head -1` picked exactly one alphabetically (libreac-transport.spec sorts
# before libreac.spec, '-' < '.'), so a plain `build-rpm.sh` silently stopped building
# libreac.so at all the moment the second spec landed. Build every *.spec found here.
TOP=$(readlink -f "${RPM_TOPDIR:-$HOME/rpmbuild}")
sh "$ROOT/packaging/make-tarball.sh" "$@"
mkdir -p "$TOP/SOURCES"
cp "$ROOT"/*.tar.gz "$TOP/SOURCES/"
# ORDER IS LOAD-BEARING. libreac-transport.spec BuildRequires pkgconfig(libreac) at this very
# version, which nothing has installed yet when both are built from one tarball -- the release
# workflow died on exactly that (v1.0.0, first dispatch). So: build libreac.spec first, stage
# its freshly built runtime + devel RPMs into $TOP/stage (rpm2cpio, no root, nothing installed
# on the host), point pkg-config at the stage and build the transport with the rpm-level
# dependency check off. The transport's own Requires/autoreq stay intact; only rpmbuild's
# BuildRequires gate is bypassed, and only because the thing it asks for is the sibling package
# built ten seconds earlier.
rpmbuild -ba --define "_topdir $TOP" "$ROOT/packaging/libreac.spec"
V=$(sed -n 's/^Version: *//p' "$ROOT/packaging/libreac.spec" | head -1)
STAGE="$TOP/stage-libreac-$V"
rm -rf "$STAGE"; mkdir -p "$STAGE"
for RPM in "$TOP"/RPMS/*/libreac-"$V"-*.rpm "$TOP"/RPMS/*/libreac-devel-"$V"-*.rpm; do
	( cd "$STAGE" && rpm2cpio "$RPM" | cpio -idm --quiet )
done
PC=$(find "$STAGE" -name libreac.pc | head -1)
[ -n "$PC" ] || { echo "build-rpm.sh: staged libreac-devel $V carries no libreac.pc"; exit 1; }
sed -i "s|^prefix=.*|prefix=$STAGE/usr|" "$PC"
PKG_CONFIG_PATH=$(dirname "$PC") rpmbuild -ba --nodeps --define "_topdir $TOP" "$ROOT/packaging/libreac-transport.spec"
