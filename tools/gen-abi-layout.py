#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# Regenerate tests/abi-layout.inc — the checked-in record of what every PUBLIC
# struct in include/reac/** looks like in memory.
#
# WHY THIS EXISTS. libreac's public structs are not opaque: reac-pw EMBEDS
# `struct reac_master` inside `struct reac_pacer` and reads its members directly.
# So a field added in the middle of one is an ABI break even though every header
# still compiles and every test still passes — the tests are rebuilt against the
# new headers and cannot see it. 2026-09-14: libreac 1.1.1 added one `int` at
# offset 1712 of `struct reac_master`, moved `headamp_src` and (through the
# embedding) `reac_pacer.recognized_box` four bytes, and the installed reac-pw
# 1.0.4 — built against 1.1.0 — read a POINTER from the wrong offset and
# dereferenced it: SEGV in sink_publish_link_props ~7 s after every start, 99
# restarts on the live rig before the rollback.
#
# The generated table is compared member by member by tests/test_abi_layout.c,
# which runs in `make test` with no gdb and no DWARF. THIS script needs gdb; the
# gate does not. Regenerating is the deliberate act that records an intended
# break, and it must move LIBREAC_ABI with it — the test refuses a table
# generated for a different ABI.
#
#   tools/gen-abi-layout.py            # rewrite tests/abi-layout.inc
#
# Run it from the top of the tree.

import os
import re
import subprocess
import sys
import tempfile

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INC = os.path.join(TOP, "include")
OUT = os.path.join(TOP, "tests", "abi-layout.inc")



# C keywords a declarator name can never be. A heuristic that returns one has
# mis-parsed, and a mis-parsed name compiles into offsetof() as garbage — so the
# generator REFUSES instead of writing it. `void (*on_session)(void *, const
# uint8_t *, unsigned int)` is the member that taught it: "the last identifier"
# picks `int` out of the parameter list.
KEYWORDS = {
    "void", "char", "short", "int", "long", "float", "double", "signed",
    "unsigned", "const", "volatile", "struct", "union", "enum", "_Bool",
    "_Atomic", "_Alignas", "restrict", "static", "inline",
}


def member_name(struct, decl):
    """The declared name in one member declaration (no trailing ';')."""
    # A function-pointer or pointer-to-array declarator wraps its name: (*name).
    fp = re.search(r"\(\s*\*+\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)", decl)
    if fp:
        return fp.group(1)
    flat = re.sub(r"\[[^\]]*\]", "", decl)
    ids = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", flat)
    if not ids or ids[-1] in KEYWORDS:
        sys.exit("cannot name a member of struct %s from %r — refusing to "
                 "guess" % (struct, decl))
    return ids[-1]


def headers():
    out = []
    for sub in ("reac", os.path.join("reac", "transport")):
        d = os.path.join(INC, sub)
        for f in sorted(os.listdir(d)):
            if f.endswith(".h"):
                out.append(os.path.join(sub, f).replace(os.sep, "/"))
    return out


def struct_names():
    names = set()
    for h in headers():
        with open(os.path.join(INC, h)) as fh:
            for line in fh:
                m = re.match(r"^struct ([A-Za-z_][A-Za-z0-9_]*) \{", line)
                if m:
                    names.add(m.group(1))
    return sorted(names)


def build_tu(tmpdir):
    src = os.path.join(tmpdir, "abi_tu.c")
    with open(src, "w") as fh:
        fh.write("#include <stddef.h>\n")
        for h in headers():
            fh.write('#include <%s>\n' % h)
        fh.write("int main(void){return 0;}\n")
    binp = os.path.join(tmpdir, "abi_tu")
    cmd = ["cc", "-g3", "-fno-eliminate-unused-debug-types", "-std=c11",
           "-D_GNU_SOURCE", "-I" + INC,
           "-I" + os.path.join(TOP, "packaging", "vendor", "reac-pw-headers"),
           "-o", binp, src]
    subprocess.run(cmd, check=True)
    return binp


def layout(binp, name):
    """(size, align, [(member, offset, size)]) for the struct's TOP level.

    A NESTED struct member closes onto its own name (`} alloc;`), so the naive
    "one line, one member" parse drops exactly the members that are themselves
    structs — `reac_master.alloc` and `reac_pacer.master`, which is to say the
    embedding that caused the incident. Depth is tracked, and the tiling check
    below refuses a table with a member missing rather than writing a short one.
    """
    r = subprocess.run(
        ["gdb", "-batch",
         "-ex", "ptype /o struct %s" % name,
         "-ex", "print sizeof(struct %s)" % name,
         "-ex", "print _Alignof(struct %s)" % name,
         binp],
        capture_output=True, text=True)
    text = r.stdout
    if "No struct type named" in text:
        return None
    size = align = None
    members = []
    depth = 0
    pending = None          # (offset, size) of a nested aggregate being opened
    for line in text.splitlines():
        stripped = line.strip()
        m = re.match(r"^\$\d+ = (\d+)$", stripped)
        if m:
            if size is None:
                size = int(m.group(1))
            elif align is None:
                align = int(m.group(1))
            continue
        opens = stripped.count("{")
        closes = stripped.count("}")
        at = depth                       # depth this line STARTS at
        depth += opens - closes

        head = re.match(r"^/\*\s*(\d+)\s*\|\s*(\d+)\s*\*/\s*(.*)$", stripped)
        if head:
            off, sz, rest = int(head.group(1)), int(head.group(2)), head.group(3)
            if rest.endswith("{"):
                if at == 1:
                    pending = (off, sz)
                continue
            if at == 1 and rest.endswith(";"):
                if ":" in rest.split(";")[0]:
                    sys.exit("struct %s has a bit-field; offsetof cannot take it"
                             % name)
                members.append((member_name(name, rest[:-1]), off, sz))
            continue

        close = re.match(r"^\}\s*([A-Za-z_][A-Za-z0-9_]*)(\[[^\]]*\])?;$",
                         stripped)
        if close and at == 2 and depth == 1:
            if pending is None:
                sys.exit("nested member %s.%s with no opening offset"
                         % (name, close.group(1)))
            members.append((close.group(1), pending[0], pending[1]))
            pending = None
        elif closes and at == 2 and depth == 1 and stripped != "}":
            sys.exit("anonymous nested member in struct %s: %r"
                     % (name, stripped))

    if size is None or align is None:
        return None

    # THE COMPLETENESS CHECK. A member the parser missed leaves a gap far wider
    # than padding can be, and a table that is silently short is worse than no
    # table at all: it would go green over the very break it exists to catch.
    prev_end = 0
    for mname, off, sz in members:
        gap = off - prev_end
        if gap < 0 or gap > align:
            sys.exit("struct %s: %d unaccounted bytes before member '%s' at "
                     "offset %d — the parser missed a member, refusing to write "
                     "a short table" % (name, gap, mname, off))
        prev_end = off + sz
    tail = size - prev_end
    if tail < 0 or tail > align:
        sys.exit("struct %s: %d unaccounted trailing bytes after the last "
                 "member — refusing to write a short table" % (name, tail))
    return size, align, members


def abi_of():
    with open(os.path.join(INC, "reac", "reac.h")) as fh:
        for line in fh:
            m = re.match(r"^#define LIBREAC_ABI (\d+)", line)
            if m:
                return int(m.group(1))
    sys.exit("LIBREAC_ABI not found in include/reac/reac.h")


def main():
    names = struct_names()
    with tempfile.TemporaryDirectory() as tmp:
        binp = build_tu(tmp)
        rows = []
        for n in names:
            lay = layout(binp, n)
            if lay is None:
                sys.exit("struct %s carries no DWARF layout — the generator "
                         "cannot vouch for it, so it refuses to write a table "
                         "that silently omits it" % n)
            rows.append((n, lay))
    with open(OUT, "w") as fh:
        fh.write("/* GENERATED by tools/gen-abi-layout.py — do not edit.\n"
                 " *\n"
                 " * The memory layout of every public struct under include/reac.\n"
                 " * tests/test_abi_layout.c compares it against this build. A\n"
                 " * difference is an ABI break; see the generator's header for the\n"
                 " * rig incident that bought this file.\n"
                 " */\n")
        fh.write("ABI_GENERATED_FOR(%d)\n" % abi_of())
        fh.write("ABI_STRUCT_COUNT(%d)\n" % len(rows))
        for n, (size, align, members) in rows:
            fh.write("\nABI_STRUCT(%s, %d, %d)\n" % (n, size, align))
            for mname, off, _sz in members:
                fh.write("ABI_MEMBER(%s, %s, %d)\n" % (n, mname, off))
    print("wrote %s: %d structs, %d members, ABI %d"
          % (os.path.relpath(OUT, TOP), len(rows),
             sum(len(r[1][2]) for r in rows), abi_of()))


if __name__ == "__main__":
    main()
