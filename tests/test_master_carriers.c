// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* THE RATE CARRIERS ARE ONE VALUE, and the ENROLL group map is the desk's.
 *
 * (1) A master declares its pace in four places — the cfea announce byte [19],
 * the ENROLL console byte (block[6], template [8]), the chanmap section marker
 * `fe <code> 00` and the scene body's `revision` (u2le at +0x14). They are not
 * four settings: a box reads the
 * announce, then caches the scene's revision and refuses to re-read the
 * body until it CHANGES, so a master whose scene contradicts its announce
 * declares one rate and records another. The pace code is 0 = 48 kHz, 1 = 96 kHz,
 * 2 = 44.1 kHz (reac_pace_code, measured 2026-09-11 on an M-200: 0x00 while
 * mastering at 48 kHz, 0x02 at 44.1 kHz, and the same desk writes scene
 * revision 0x0000 at 48 kHz and 0x0002 at 44.1 kHz).
 *
 * WHAT WENT WRONG AND WHY A 48/96 kHz TEST COULD NOT SEE IT: the scene revision
 * was stamped `cfg.console_field ? 1 : 0`. A boolean squash is INVISIBLE while
 * the field only ever holds 0 or 1 — every existing test runs at 48 or 96 kHz —
 * and turns the 44.1 kHz code 2 into 1, i.e. into the 96 kHz class, so a 44.1 kHz
 * master announced 44.1 and recorded 96. The 44.1 kHz case below is the one that
 * goes red against it; the 0/1 cases are the control that the fix changed nothing
 * at the rates the rig runs.
 *
 * (2) The ENROLL group map (cdea 01 03 000d, block[7:17]) is compared against the
 * bytes real desks put on the wire, per box width.
 *
 * Pure: no socket, no thread, no rig. */

#include <reac/reac.h>
#include <reac/reac_master.h>
#include <reac/reac_ctrlblk.h>

#include <stdio.h>
#include <string.h>

#define CHK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL: %s (line %d)\n", #x, __LINE__); return 1; } } while (0)

/* Our own NIC's address — a master emits from the L2 source, never a cloned
 * desk MAC (reac_mac.h). */
static const uint8_t OUR[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };

/* The ENROLL template index of the console byte; block[6] = template[8]. */
#define ENROLL_CONSOLE 8

/* A 34-byte [type|block] template closes when its 32-byte block sums to 0 mod
 * 256 (reac_ctrl_block_cksum_stamp). Checked by hand here so the test needs no
 * frame around the template. */
static int block_closes(const uint8_t tmpl[34])
{
	unsigned sum = 0;
	for (int i = 0; i < REAC_CTRL_BLOCK_LEN; i++)
		sum += tmpl[2 + i];
	return (sum & 0xff) == 0;
}

static int carriers(int fps, uint8_t code)
{
	struct reac_console_cfg cfg = { .out_channels = 8, .console_field = code };
	struct reac_master m;
	reac_master_init(&m, OUR, &cfg, fps);

	/* ONE value, four places. */
	CHK(m.announce_blk[19] == code);                  /* cfea [19]            */
	CHK(m.enroll_blk[ENROLL_CONSOLE] == code);        /* ENROLL console byte  */
	CHK(m.scene[REAC_SCENE_REVISION_OFF] == code);    /* scene revision, u2le */
	CHK(m.scene[REAC_SCENE_REVISION_OFF + 1] == 0x00);

	/* The chanmap section marker `fe <code> 00`. The sweep is a 49-window ring and
	 * only some windows hold the 0xfe slot, so count them: an assertion that never
	 * found a marker would pass on a generator that had stopped emitting one. */
	{
		int markers = 0;
		for (int w = 0; w < m.chanmap_nframes; w++)
			for (int sl = 0; sl < 8; sl++) {
				const uint8_t *t = m.chanmap[w] + 7 + sl * 3;
				if (t[0] != 0xfe)
					continue;
				markers++;
				CHK(t[1] == code);
				CHK(t[2] == 0x00);
			}
		CHK(markers == 8);   /* the 0xfe position appears in 8 of the 49 windows */
	}

	/* Every block that carries one still closes its own checksum. */
	CHK(block_closes(m.announce_blk));
	CHK(block_closes(m.enroll_blk));
	return 0;
}

int main(void)
{
	/* The pace code is derived from the frame rate in ONE place. Pin the mapping
	 * here as well as at its two transport call sites: a master built at 3675
	 * frames/s and one built from reac_pace_code(3675) must be the same master. */
	CHK(reac_pace_code(3675) == 2);   /* 44.1 kHz */
	CHK(reac_pace_code(4000) == 0);   /* 48 kHz   */
	CHK(reac_pace_code(8000) == 1);   /* 96 kHz   */

	if (carriers(4000, reac_pace_code(4000))) return 1;   /* 48 kHz   */
	if (carriers(8000, reac_pace_code(8000))) return 1;   /* 96 kHz   */
	if (carriers(3675, reac_pace_code(3675))) return 1;   /* 44.1 kHz */

	/* NEGATIVE CONTROL: the bytes MOVE with the code. Asserts that all read a
	 * constant would pass on a generator that had stopped reading the pace code at
	 * all, which is exactly the defect this file exists for. */
	{
		struct reac_console_cfg a = { .out_channels = 8, .console_field = 0 };
		struct reac_console_cfg b = { .out_channels = 8, .console_field = 2 };
		struct reac_master ma, mb;
		reac_master_init(&ma, OUR, &a, 4000);
		reac_master_init(&mb, OUR, &b, 3675);
		CHK(ma.announce_blk[19] != mb.announce_blk[19]);
		CHK(ma.enroll_blk[ENROLL_CONSOLE] != mb.enroll_blk[ENROLL_CONSOLE]);
		CHK(ma.scene[REAC_SCENE_REVISION_OFF] != mb.scene[REAC_SCENE_REVISION_OFF]);
		CHK(memcmp(ma.chanmap[0], mb.chanmap[0], 34) != 0 ||
		    memcmp(ma.chanmap[6], mb.chanmap[6], 34) != 0);
	}

	/* ---- THE ENROLL GROUP MAP, AS REAL DESKS SEND IT --------------------
	 *
	 * Goldens, byte for byte, from reac-captures via tools/group_map_scan:
	 *
	 *   8-input  S-0808  <- M-200  c9:cc:03   18 frames, ...__ctl2.pcap
	 *   32-input S-4000S <- M-200  c9:cc:03    2 frames,
	 *                       ...matrix-m200-s4000-2026-07-24.pcap
	 *
	 * A real S-4000S under a static 1 x 0x41 stayed at 8 channels upstream
	 * (340 B) and widened to 32 (1204 B) only under 4 x 0x41 — both lengths are in
	 * that one capture, which is what makes these bytes a gate and not decoration. */
	{
		static const struct { int in_ch; const char *tmpl; } WIRE[] = {
			{  8, "cdea0103000d100400410000000000c3c3c3c300000000000000000000000000008e" },
			{ 32, "cdea0103000d100400414141410000000000c3000000000000000000000000000014" },
		};
		for (size_t i = 0; i < sizeof WIRE / sizeof WIRE[0]; i++) {
			struct reac_console_cfg cfg = { .out_channels = 8, .console_field = 0 };
			struct reac_master m;
			char got[80];
			reac_master_init(&m, OUR, &cfg, 4000);
			reac_master_set_box(&m, WIRE[i].in_ch, 8, 0x00);
			for (int j = 0; j < 34; j++)
				sprintf(got + j * 2, "%02x", m.enroll_blk[j]);
			if (strcmp(got, WIRE[i].tmpl) != 0) {
				fprintf(stderr, "FAIL: %d-input ENROLL\n  want %s\n  got  %s\n",
				        WIRE[i].in_ch, WIRE[i].tmpl, got);
				return 1;
			}
			CHK(block_closes(m.enroll_blk));
		}
	}

	printf("OK: the pace code reaches all four carriers (cfea[19], ENROLL[8], the "
	       "chanmap section marker and the scene revision) at 44.1/48/96 kHz, and the "
	       "ENROLL group map is byte-identical to the M-200 enrols captured for 8- and "
	       "32-input boxes\n");
	return 0;
}
