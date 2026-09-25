#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""Generate a facts header from a live reac-protocol checkout.

Called from the Makefile, never by hand: writes exactly one file (the path
given as OUT) and touches nothing inside the reac-protocol checkout. Every
byte comes from reac-protocol's own gen-facts.py, so the two projects can
never disagree about what the file says.

Two outputs:

  gen-facts-header.py SCHEMA GEN_FACTS_PY OUT
      tests/reac_facts_assert.h: the _Static_assert block that binds
      libreac's own macros to spec/protocol-facts.yaml.

  gen-facts-header.py SCHEMA GEN_FACTS_PY OUT --groups ID[,ID...] --guard G
      A public header that DEFINES the named groups' facts, exactly as
      reac-protocol's generated/reac_facts.h spells them, and nothing else.
      This is how a fact libreac reads instead of declaring moves in: one
      group at a time. The whole reac_facts.h cannot be included yet, since
      it redefines several macros libreac still declares by hand.
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
    ap.add_argument("--groups", help="comma-separated group ids to define")
    ap.add_argument("--guard", help="include guard for the --groups header")
    args = ap.parse_args()

    gen_facts = load_gen_facts(args.gen_facts_py)
    schema = yaml.safe_load(args.schema.read_text())
    if args.groups:
        if not args.guard:
            ap.error("--groups needs --guard")
        want = args.groups.split(",")
        groups = [g for g in schema["groups"] if g["id"] in want]
        missing = sorted(set(want) - {g["id"] for g in groups})
        if missing:
            # A group the schema does not have is a schema libreac is not built
            # against, and an empty header would say nothing about it.
            sys.exit(f"gen-facts-header.py: {args.schema} has no group(s) {missing}")
        content = gen_facts.emit_h(dict(schema, groups=groups,
                                        meta=dict(schema["meta"], guard=args.guard)))
    else:
        content = gen_facts.emit_assert_h(schema)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(content)
    return 0


if __name__ == "__main__":
    sys.exit(main())
