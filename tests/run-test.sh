#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# EVERY TEST ENDS WITH A VERDICT. `make test` runs each test through this:
#
#   tests/run-test.sh ./test_x [args...]
#
#   0      PASS
#   77     SKIP: the test proved this environment cannot host it (a netns/veth test
#          on a GitHub runner). Printed loudly and counted as not-a-failure; the
#          test's own output says NOTHING WAS TESTED. (automake's skip code; reac-pw
#          #114: a namespace body's rc is a verdict.)
#   124    TIMEOUT: the test ran past $REAC_TEST_TIMEOUT seconds (default 120) and was
#          killed. A hang is RED. It used to stall CI until the run was cancelled:
#          every libreac `test` job from 2026-09-25 13:18Z hung in test_rate_detect's
#          blocking send() until someone force-cancelled it.
#   other  FAIL, passed through unchanged (2 is a test's own NOT A RESULT).
set -u
t=${REAC_TEST_TIMEOUT:-120}
name=$1
timeout --kill-after=10 "$t" "$@"
rc=$?
case $rc in
	0)   exit 0 ;;
	77)  echo "SKIP: $name — exit 77, this environment cannot host it (see its output above)"
	     [ -n "${REAC_SKIP_LOG:-}" ] && echo "$name" >> "$REAC_SKIP_LOG"
	     exit 0 ;;
	124|137)
	     echo "FAIL: $name TIMED OUT after ${t}s and was killed — a hang is red, never a stall" >&2
	     exit 124 ;;
	*)   echo "FAIL: $name exited $rc" >&2
	     exit "$rc" ;;
esac
