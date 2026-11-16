#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE PYTHON BINDING'S OWN TESTS RUN, AGAINST THE LIBRARY THIS TREE BUILDS.
# python/test_libreac.py was written and wired into nothing: no Makefile target and no
# workflow ran it, so the binding could break with every check green. This builds a shared
# object from every src/*.c (with the soname the binding loads, read from reac.h), puts it
# alone on LD_LIBRARY_PATH, and runs the file (it carries its own runner; pytest is not needed).
# A missing compiler or Python is a failure: both are required for libreac's build and its binding.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2 -std=c11 -Wall -Wextra}
PYTHON=${PYTHON:-python3}
B=${REAC_PYTHON_BINDING_BUILD:-"$ROOT/build/python-binding"}

abi=$(awk '/^#define LIBREAC_ABI[ 	]/ { print $3; exit }' "$ROOT/include/reac/reac.h")
version=$(awk '/^#define LIBREAC_VERSION_(MAJOR|MINOR|PATCH)/ { v = v (v == "" ? "" : ".") $3 } END { print v }' "$ROOT/include/reac/reac.h")
[ -n "$abi" ] && [ -n "$version" ] || { echo "python-binding: cannot read ABI or version from reac.h" >&2; exit 2; }

mkdir -p "$B"
"$CC" $CFLAGS -fPIC -shared -I"$ROOT/include" \
	-Wl,-soname,"libreac.so.$abi" "$ROOT"/src/*.c -lm -o "$B/libreac.so.$version"
ln -sf "libreac.so.$version" "$B/libreac.so.$abi"

cd "$B"
exec env -i PATH="$PATH" LD_LIBRARY_PATH="$B" "$PYTHON" "$ROOT/python/test_libreac.py"
