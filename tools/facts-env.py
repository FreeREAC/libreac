#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""Print the protocol's scalar facts as shell exports, from tests/facts/reac_facts.h.

    eval "$(tools/facts-env.py)"      exports FACT_<NAME>=<value>

The FACT_ prefix is deliberate: libreac and its daemons read REAC_* / REACPW_* knobs, and an
exported fact must never be mistaken for one.
"""
import pathlib
import re
import sys

DEFINE = re.compile(r"^#define REAC_([A-Z0-9_]+)\s+(\S.*?)\s*(?:/\*.*)?$", re.M)
HEADER = pathlib.Path(__file__).resolve().parent.parent / "tests" / "facts" / "reac_facts.h"

for name, val in sorted(DEFINE.findall(HEADER.read_text())):
    try:
        print(f"export FACT_{name}={int(val.strip(), 0)}")
    except ValueError:
        pass
