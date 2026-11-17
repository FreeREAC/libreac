# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# shellcheck shell=bash
#
# Sourced by the shell tests: every REAC protocol number as FACT_<NAME>, read from
# tests/facts/reac_facts.h (held to reac-protocol by `make facts-drift-check`) by
# tools/facts-env.py. Exported, so an `unshare ... bash -s` body inherits them.
# `--rate "$FACT_SAMPLE_RATE_96K"`, never 96000.
eval "$(python3 "$(dirname "${BASH_SOURCE[0]}")/../tools/facts-env.py")" || {
	echo "FAIL: tools/facts-env.py could not read the protocol facts"; exit 1; }
