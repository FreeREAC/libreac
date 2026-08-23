#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""Generate tests/reac_facts_assert.h from a live reac-protocol checkout.

Called from the Makefile, never by hand: writes exactly one file (the path
given as OUT) and touches nothing inside the reac-protocol checkout — the
_Static_assert block that binds libreac's own macros to
spec/protocol-facts.yaml, built by reac-protocol's own gen-facts.py so the
two projects can never disagree about what that block says.

Usage: gen-facts-header.py SCHEMA GEN_FACTS_PY OUT
"""
import argparse
import importlib.util
import pathlib
import sys

import yaml


def load_gen_facts(gen_facts_py):
    spec = importlib.util.spec_from_file_location("_reac_gen_facts", gen_facts_py)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("schema", type=pathlib.Path)
    ap.add_argument("gen_facts_py", type=pathlib.Path)
    ap.add_argument("out", type=pathlib.Path)
    args = ap.parse_args()

    gen_facts = load_gen_facts(args.gen_facts_py)
    schema = yaml.safe_load(args.schema.read_text())
    content = gen_facts.emit_assert_h(schema)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(content)
    return 0


if __name__ == "__main__":
    sys.exit(main())
