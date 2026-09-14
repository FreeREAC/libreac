// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* THE PUBLIC STRUCTS' MEMORY LAYOUT IS AN INTERFACE, AND NOTHING ELSE WAS
 * WATCHING IT.
 *
 * libreac's structs are not opaque. reac-pw EMBEDS `struct reac_master` inside
 * `struct reac_pacer` and reads its members directly, so where each member sits
 * is as much a promise as any function signature — and it is the one promise a
 * suite cannot notice being broken, because every test is REBUILT against the
 * new headers and agrees with itself perfectly.
 *
 * WHAT THAT COST, 2026-09-14. libreac 1.1.1 added one `int` at offset 1712 of
 * `struct reac_master` — a per-cycle cursor, entirely internal — and moved every
 * member behind it four bytes: `headamp_src` 14304 -> 14308, `box_mac` 14344 ->
 * 14348, and through the embedding `reac_pacer.recognized_box` 15232 -> 15236.
 * `struct reac_master` went 14456 -> 14464 bytes and `struct reac_pacer` 24312 ->
 * 24320. The installed reac-pw 1.0.4, built against 1.1.0, then read a POINTER
 * from the wrong offset and dereferenced it: SEGV in sink_publish_link_props
 * about 7 s after every start, 99 restarts on the live rig, rolled back. The
 * library's own `make test` was green throughout, and so was the fake-box test
 * that shipped in the same commit.
 *
 * WHAT THIS GATE IS. `tests/abi-layout.inc` records sizeof, _Alignof and the
 * offset of every member of all 61 public structs, generated from DWARF by
 * `tools/gen-abi-layout.py`. This file re-measures them with plain `offsetof`
 * against the headers as they are now. Any difference is red.
 *
 * AND THE ONE DOOR OUT. A deliberate break is landed by bumping LIBREAC_ABI (the
 * soname's major) and regenerating the table in the SAME change; the table
 * records the ABI it was generated for, so a bump without a regeneration and a
 * regeneration without a bump are both red. There is no way to move a layout and
 * keep the file quiet.
 *
 * The generator needs gdb. This does not: it is plain C over a checked-in table,
 * so it runs wherever the library builds. */

#include <reac/reac.h>

/* Every public header, because the table covers every public struct. */
#include <reac/pcap_source.h>
#include <reac/reac_arbitration.h>
#include <reac/reac_boxreg.h>
#include <reac/reac_braid.h>
#include <reac/reac_capture.h>
#include <reac/reac_cfg.h>
#include <reac/reac_clock.h>
#include <reac/reac_ctrl.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_decode.h>
#include <reac/reac_disco.h>
#include <reac/reac_encode.h>
#include <reac/reac_envflag.h>
#include <reac/reac_fsm.h>
#include <reac/reac_grant.h>
#include <reac/reac_headamp_tx.h>
#include <reac/reac_hunt.h>
#include <reac/reac_identity.h>
#include <reac/reac_link.h>
#include <reac/reac_link_state.h>
#include <reac/reac_macaddr.h>
#include <reac/reac_master.h>
#include <reac/reac_master_fsm.h>
#include <reac/reac_ports.h>
#include <reac/reac_role.h>
#include <reac/reac_sample.h>
#include <reac/reac_scene_body.h>
#include <reac/reac_slots.h>
#include <reac/reac_upstream.h>
#include <reac/transport/reac_carrier.h>
#include <reac/transport/reac_conf.h>
#include <reac/transport/reac_handle.h>
#include <reac/transport/reac_ifname.h>
#include <reac/transport/reac_ifscan.h>
#include <reac/transport/reac_linkmon.h>
#include <reac/transport/reac_mac.h>
#include <reac/transport/reac_pace_watch.h>
#include <reac/transport/reac_pacer.h>
#include <reac/transport/reac_ring.h>
#include <reac/transport/reac_role_swap.h>
#include <reac/transport/reac_rt.h>
#include <reac/transport/reac_rx.h>
#include <reac/transport/reac_seglock.h>
#include <reac/transport/reac_segment_ident.h>
#include <reac/transport/reac_slave.h>
#include <reac/transport/reac_tap.h>
#include <reac/transport/reac_topo.h>
#include <reac/transport/reac_tx.h>
#include <reac/transport/reac_vlan.h>

#include <stddef.h>
#include <stdio.h>

static int fails;
static int structs;
static int members;

static void bad(const char *what, const char *field, unsigned long got,
                unsigned long want)
{
	fprintf(stderr,
	        "ABI LAYOUT MOVED: %s %s is %lu, the table records %lu\n",
	        what, field, got, want);
	fails++;
}

/* THE TABLE IS THE ABI IT WAS GENERATED FOR. Regenerating for a new soname major
 * is the deliberate act; a bump with a stale table is a break nobody recorded. */
#define ABI_GENERATED_FOR(abi)                                                \
	static const int table_abi = (abi);
#define ABI_STRUCT_COUNT(n)                                                   \
	static const int table_structs = (n);
#define ABI_STRUCT(name, size, align)
#define ABI_MEMBER(name, member, off)
#include "abi-layout.inc"
#undef ABI_GENERATED_FOR
#undef ABI_STRUCT_COUNT
#undef ABI_STRUCT
#undef ABI_MEMBER

int main(void)
{
	if (table_abi != LIBREAC_ABI) {
		fprintf(stderr,
		        "ABI MISMATCH: tests/abi-layout.inc was generated for LIBREAC_ABI "
		        "%d, this tree is %d.\n"
		        "  Moving the soname's major is how a deliberate layout break is "
		        "landed, and the table has to move WITH it — in the same change, "
		        "so the diff is the record of what broke.\n"
		        "  Regenerate: tools/gen-abi-layout.py\n",
		        table_abi, LIBREAC_ABI);
		return 1;
	}

#define ABI_GENERATED_FOR(abi)
#define ABI_STRUCT_COUNT(n)
#define ABI_STRUCT(name, size, align)                                         \
	structs++;                                                                \
	if (sizeof(struct name) != (size_t)(size))                                \
		bad("struct " #name, "sizeof",                                        \
		    (unsigned long)sizeof(struct name), (unsigned long)(size));       \
	if (_Alignof(struct name) != (size_t)(align))                             \
		bad("struct " #name, "_Alignof",                                      \
		    (unsigned long)_Alignof(struct name), (unsigned long)(align));
#define ABI_MEMBER(name, member, off)                                         \
	members++;                                                                \
	if (offsetof(struct name, member) != (size_t)(off))                       \
		bad("struct " #name, "." #member,                                     \
		    (unsigned long)offsetof(struct name, member),                     \
		    (unsigned long)(off));
#include "abi-layout.inc"
#undef ABI_GENERATED_FOR
#undef ABI_STRUCT_COUNT
#undef ABI_STRUCT
#undef ABI_MEMBER

	/* A COUNT IS NOT COVERAGE, BUT A DROPPED TABLE IS A SILENT PASS. If the
	 * include ever expanded to nothing — a renamed file, a botched generation —
	 * every check above would vanish and this would report success over an
	 * empty scan. Require the table to be the size it says it is. */
	if (structs != table_structs || structs == 0 || members == 0) {
		fprintf(stderr,
		        "ABI TABLE DID NOT RUN: %d structs / %d members checked, the "
		        "table declares %d structs. An empty scan is not a pass.\n",
		        structs, members, table_structs);
		return 1;
	}

	if (fails) {
		fprintf(stderr,
		        "\n%d layout difference(s). These structs are PUBLIC and reac-pw "
		        "embeds struct reac_master inside struct reac_pacer, so a member "
		        "that moved breaks every binary built against the old headers — "
		        "silently, at whatever offset it happens to read (rig, "
		        "2026-09-14: a moved pointer, SEGV in sink_publish_link_props, 99 "
		        "restarts).\n"
		        "  Keep the new state private (a static, a computed value, an "
		        "existing field) and the layout does not move at all.\n"
		        "  If the break is intended: bump LIBREAC_ABI in "
		        "include/reac/reac.h, move the sonames with it, and regenerate "
		        "with tools/gen-abi-layout.py in the SAME change.\n", fails);
		return 1;
	}

	printf("OK: all %d public structs hold the layout the checked-in table "
	       "records — %d member offsets, sizeof and _Alignof, at LIBREAC_ABI %d. "
	       "A field added to a public struct moves every member behind it and "
	       "breaks binaries built against the old headers; nothing else in this "
	       "suite can see that, because every test is rebuilt with the change\n",
	       structs, members, LIBREAC_ABI);
	return 0;
}
