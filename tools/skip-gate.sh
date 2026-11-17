#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# usage: skip-gate.sh [SKIP_LOG]     (default build/skipped.txt)
#
# Fails when any test other than the allowed ones skipped (exit 77, "this environment cannot
# host it"). tests/run-test.sh reports a skip and appends the test's name to the log; on a
# machine that cannot run a namespace test that is honest, but the privileged CI job exists to
# run exactly those, so there a skip is a test that did not run. Allowed: tools/ops-slugs.sh,
# which needs the freereac-ops checkout and is a developer gate, absent from every CI run. The python
# binding's grammar arm is NOT allowed: CI generates the parser and installs the Kaitai runtime, so a
# skip of tests/python-binding.sh there means the arm did not run.
#
# A MISSING log is a failure, an empty one a pass: `make test` creates the log empty before it
# runs anything, so no file means the suite did not run (or ran elsewhere) and "nothing skipped"
# would be a claim nothing made.
log=${1:-build/skipped.txt}
[ -e "$log" ] || { echo "FAIL: no skip log at $log: make test did not run here, so nothing can be said about skips" >&2; exit 1; }
[ -s "$log" ] || exit 0
bad=$(grep -v -x -e 'tools/ops-slugs.sh' "$log" | sort -u)
[ -z "$bad" ] && exit 0
echo "FAIL: these tests skipped (exit 77) where every one of them must run:" >&2
echo "$bad" | sed 's/^/  /' >&2
exit 1
