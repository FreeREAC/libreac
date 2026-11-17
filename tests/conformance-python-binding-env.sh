#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE PYTHON BINDING'S RUN KEEPS REAC_PROTOCOL, AND A SKIPPED GRAMMAR ARM IS A SKIP.
# tests/python-binding.sh runs the binding's tests under `env -i`. That used to drop
# REAC_PROTOCOL, so the grammar arm looked only at its default path and printed SKIP while the
# run reported a pass. Point REAC_PROTOCOL at a path that cannot hold a grammar: the run must
# (1) name that path in the reason it gives, which it can only do if the variable arrived, and
# (2) end with exit 77, not 0 -- a skipped arm is not a pass.
set -u
cd "$(dirname "$0")/.." || exit 2
d=$(mktemp -d) || exit 2
trap 'rm -rf "$d"' EXIT
bogus=/no/such/reac-protocol-checkout
REAC_PROTOCOL=$bogus REAC_PYTHON_BINDING_BUILD=$d/b sh tests/python-binding.sh > "$d/out" 2>&1
rc=$?
cat "$d/out"
grep -q "SKIP test_the_grammar_accepts_every_frame_libreac_builds" "$d/out" || {
	echo "FAIL: the grammar arm did not report a SKIP (is the grammar really unreachable?)" >&2; exit 1; }
grep -q "$bogus" "$d/out" || {
	echo "FAIL: REAC_PROTOCOL did not reach the python run: the skip reason never names $bogus" >&2; exit 1; }
[ "$rc" -eq 77 ] || { echo "FAIL: a skipped grammar arm must end the run with 77, got $rc" >&2; exit 1; }
echo "ok: python-binding.sh forwards REAC_PROTOCOL and a skipped arm exits 77"
