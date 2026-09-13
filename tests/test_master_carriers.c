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

/* The announce's enrolled-box count: u2 BIG-endian at block[18:20], template
 * [20:22] (2026-09-13 corpus pass, 17 040 announces — the high byte has never
 * been non-zero, so reading only the low one would pass on a master that had
 * stopped writing the field's other half). */
static uint16_t ann_box_count(const struct reac_master *m)
{
	return (uint16_t)((m->announce_blk[20] << 8) | m->announce_blk[21]);
}

/* THE ANNOUNCE'S WIDTH AND COUNT, AGAINST A REAL DESK'S OWN TIMELINE.
 *
 * Measured off an M-200 driving an S-1608 at 44.1 kHz, bouncing the box
 * (reac-captures m200-enrol-441k-2026-09-13/analysis.md, timeline):
 *
 *   box absent          cfea width 0x08
 *   box's commit report cfea width back to 0x10, box_count STILL 0
 *   the grant burst
 *   +0.5 s              box_count 0 -> 1
 *
 * So the width tracks RECOGNITION and the count tracks the GRANT, and they are
 * two different instants ~2 s apart. A master that raises the count when the box
 * declares itself announces a granted box through the whole recognized-but-
 * ungranted dwell, which is the window a real desk holds open — that is the
 * defect this drive exists to catch, and it is invisible to any assertion taken
 * only at the end state, where both readings agree.
 *
 * Drives the FSM exactly as reac_pacer does: one reac_master_next per emitted
 * frame, RX events fed in. No socket, no thread, no rig. */
static int announce_width_and_count(void)
{
	static const uint8_t BOX[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x3b };
	uint8_t join[32];
	struct reac_console_cfg cfg = { .out_channels = 8,
	                                .console_field = reac_pace_code(4000) };
	struct reac_master m;
	uint16_t c;
	int ix;
	long guard;

	memset(join, 0, sizeof join);
	reac_master_init(&m, OUR, &cfg, 4000);

	/* NO BOX: the idle width every captured desk announces while unlinked. */
	CHK(m.announce_blk[18] == 0x08);
	CHK(ann_box_count(&m) == 0);
	CHK(block_closes(m.announce_blk));

	/* The JOIN is held until the scene transfer completes; then GRANTING opens
	 * the dwell. The guard is checked, not assumed: a drive that never reached
	 * GRANTING would otherwise assert the idle state all the way down. */
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX, join) == 0);
	for (guard = 0; m.state != REAC_M_GRANTING && guard < 8L * m.cycle_len; guard++)
		(void)reac_master_next(&m, &c, &ix);
	CHK(m.state == REAC_M_GRANTING);
	CHK(m.announce_blk[18] == 0x08);   /* a cold JOIN carries no width */
	CHK(ann_box_count(&m) == 0);

	/* The box's commit report: the width moves, the count does NOT. */
	reac_master_set_box(&m, 16, 8, 0x20);
	CHK(reac_master_has_box(&m) == 1);
	CHK(m.announce_blk[18] == 0x10);
	CHK(ann_box_count(&m) == 0);
	CHK(m.announce_blk[19] == reac_pace_code(4000));   /* the pace survives it */
	CHK(block_closes(m.announce_blk));

	/* The burst goes out and the box heartbeats: GRANTED. */
	for (guard = 0; m.state == REAC_M_GRANTING && guard < 400000L; guard++) {
		(void)reac_master_next(&m, &c, &ix);
		if (guard % 500 == 0)
			(void)reac_master_rx(&m, REAC_M_RX_BOX_HEARTBEAT, BOX, NULL);
	}
	CHK(m.state == REAC_M_ESTABLISHED);
	CHK(ann_box_count(&m) == 1);       /* 0 -> 1 at the grant, and only there */
	CHK(m.announce_blk[18] == 0x10);   /* no width collapse at latch */
	CHK(block_closes(m.announce_blk));

	/* The box goes away: the peer-gone budget drains, and everything we advertise
	 * about "the box" goes with it. */
	for (guard = 0; m.state == REAC_M_ESTABLISHED && guard < 400000L; guard++)
		(void)reac_master_next(&m, &c, &ix);
	CHK(m.state == REAC_M_PROBING);
	CHK(ann_box_count(&m) == 0);       /* 1 -> 0 on the drop */
	CHK(m.announce_blk[18] == 0x08);
	CHK(block_closes(m.announce_blk));

	/* The width is the ENROLLED BOX's declared input width, whatever it is —
	 * a constant 0x10 would pass everything above. */
	{
		static const uint8_t W[] = { 8, 16, 32 };
		for (size_t i = 0; i < sizeof W / sizeof W[0]; i++) {
			struct reac_master mw;
			reac_master_init(&mw, OUR, &cfg, 4000);
			reac_master_set_box(&mw, W[i], 8, 0x00);
			CHK(mw.announce_blk[18] == W[i]);
			CHK(block_closes(mw.announce_blk));
		}
	}
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

	/* THE PACE CODE THE WIRE CARRIES, per rate, as literals — the assertions above
	 * compare the announce against reac_pace_code's own answer, so all three would
	 * still agree if that one function drifted. These are the measured values:
	 * 0x00 at 48 kHz and 0x02 at 44.1 kHz off one M-200 MAC, 0x01 at a measured
	 * 8005 pps off an S-1608 and an S-4000S (17 040 announces, 105 capture files,
	 * reac-captures analysis/2026-09-13-announce-bytes-and-headamp-base.md). */
	{
		static const struct { int fps; uint8_t code; } RATE[] = {
			{ 4000, 0x00 },   /* 48 kHz   */
			{ 8000, 0x01 },   /* 96 kHz   */
			{ 3675, 0x02 },   /* 44.1 kHz */
		};
		for (size_t i = 0; i < sizeof RATE / sizeof RATE[0]; i++) {
			struct reac_console_cfg cfg = {
				.out_channels  = 8,
				.console_field = reac_pace_code(RATE[i].fps),
			};
			struct reac_master m;
			reac_master_init(&m, OUR, &cfg, RATE[i].fps);
			CHK(m.announce_blk[19] == RATE[i].code);
			CHK(block_closes(m.announce_blk));
		}
	}

	if (announce_width_and_count()) return 1;

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
	 *   16-input S-1608  <- M-200  c9:cc:03 + M-200i c9:cc:04
	 *                                        13 frames over 6 captures, and in
	 *                                        three of them the S-1608 is the ONLY
	 *                                        box on the wire
	 *   32-input S-4000S <- M-200  c9:cc:03    2 frames,
	 *                       ...matrix-m200-s4000-2026-07-24.pcap
	 *
	 * The 32-input row is why the rule is not "one group, always": a real S-4000S
	 * under a static 1 x 0x41 stayed at 8 channels upstream
	 * (340 B) and widened to 32 (1204 B) only under 4 x 0x41 — both lengths are in
	 * that one capture, which is what makes these bytes a gate and not decoration. */
	{
		static const struct { int in_ch; const char *tmpl; } WIRE[] = {
			{  8, "cdea0103000d100400410000000000c3c3c3c300000000000000000000000000008e" },
			/* THE FINDING: the desks send a 16-input box the SAME map as an
			 * 8-input one. libreac sent 2 x 0x41 here, a shape that appears in
			 * zero frames of the corpus (set_enroll_width carries the numbers). */
			{ 16, "cdea0103000d100400410000000000c3c3c3c300000000000000000000000000008e" },
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
	       "chanmap section marker and the scene revision) at 44.1/48/96 kHz and reads "
	       "0x00/0x01/0x02 there; the announce width tracks the recognized box (0x08 "
	       "idle, 8/16/32) and the box count rises 0->1 only at the grant and falls "
	       "back on the drop, as the M-200 bounce timeline has it; and the ENROLL group "
	       "map is byte-identical to the M-200 enrols captured for 8-, 16- and 32-input "
	       "boxes\n");
	return 0;
}
