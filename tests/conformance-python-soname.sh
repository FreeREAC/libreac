#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE PYTHON BINDING LOADS THE ABI SONAME, NOT AN OLD ONE. The C header is the
# one source of the ABI. Build a shared object whose directory has only its
# versioned file and the ABI-soname link. Change into that directory before
# import: package builds make a root `libreac.so` that Python can mistake for an
# extension module. Then require ctypes to name the ABI link. An old literal
# soname, or a versioned-name load, cannot pass this test.
# This is a real shared object made from every src/*.c. ctypes calls
# reac_version(), so success proves the import loaded this object rather than
# just accepting the expected name.
#
# Exit 0 passes. A missing compiler or Python is a failure: both are required
# for libreac's build and its ctypes binding.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2 -std=c11 -Wall -Wextra}
PYTHON=${PYTHON:-python3}
B=${REAC_PYTHON_SONAME_BUILD:-"$ROOT/build/python-soname"}

abi=$(awk '/^#define LIBREAC_ABI[ 	]/ { print $3; exit }' "$ROOT/include/reac/reac.h")
version=$(awk '/^#define LIBREAC_VERSION_(MAJOR|MINOR|PATCH)/ { v = v (v == "" ? "" : ".") $3 } END { print v }' "$ROOT/include/reac/reac.h")
[ -n "$abi" ] && [ -n "$version" ] || {
	echo "conformance-python-soname: cannot read ABI or version from reac.h" >&2
	exit 2
}

rm -rf "$B"
mkdir -p "$B"
trap 'rm -rf "$B"' EXIT
"$CC" $CFLAGS -fPIC -shared -I"$ROOT/include" \
	-Wl,-soname,"libreac.so.$abi" "$ROOT"/src/*.c -lm -o "$B/libreac.so.$version"
ln -s "libreac.so.$version" "$B/libreac.so.$abi"
[ ! -e "$B/libreac.so" ] && [ ! -e "$B/libreac.so.1" ] || {
	echo "conformance-python-soname: test directory contains a development or old soname link" >&2
	exit 2
}

out=$(cd "$B" && env -i PATH="$PATH" LD_LIBRARY_PATH="$B" PYTHONPATH="$ROOT/python" \
	LIBREAC_TEST_SONAME="libreac.so.$abi" "$PYTHON" -c \
	'import os, libreac; assert libreac._lib._name == os.environ["LIBREAC_TEST_SONAME"], libreac._lib._name; print(libreac.version())')
[ "$out" = "$version" ] || {
	echo "conformance-python-soname: binding reported '$out', expected '$version'" >&2
	exit 1
}
echo "conformance-python-soname: PASS (libreac.so.$abi only)"
