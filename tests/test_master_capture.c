// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* A DESK CAPTURES AN ALREADY-LINKED, SILENT BOX WITH A COMPLETED SCENE TRANSFER,
 * and the box answers with its state-4 COMMIT REPORT, not a cold connect.
 *
 * MEASURED, 2026-09-14, `reac-captures desk-arrival-q4-2026-09-14/desk-arrival-slice.pcap`
 * (M-200 `00:40:ab:c9:cc:03` unplugged and replugged at 44.1 kHz; S-4000S-3208
 * `00:40:ab:c4:08:bc` linked to the switch throughout, its cable never touched):
 *
 *   t=…390.849028  desk's first frame back — FILLER
 *   t=…390.869148  cfea: box_in_width 0x08, box_count 0x0000, pace 0x02   (+20.1 ms)
 *   t=…390.850151  338 MIDDLE chunks and a LAST with NO FIRST — the tail of a
 *                  transfer begun while unplugged. THE BOX DOES NOT ANSWER IT.
 *   t=…393.537566  FIRST, declaring 0x22c8      counter 46839
 *   t=…394.218879  the 341st MIDDLE             counter 49343   (HEAD + 2504 slots)
 *   t=…394.220840  LAST                         counter 49350   (HEAD + 2511 slots)
 *   t=…394.229741  the box's COMMIT REPORT `cdea 01 03 0010 84` (+8.4 ms after LAST)
 *   t=…394.439302  the desk's ENROLL group map  (+209.6 ms after the report)
 *   t=…395.927676  the box's `cdea 04 03` burst (+1.489 s after the group map)
 *   t=…397.433060  the desk's grant             (+1.505 s after the burst)
 *
 * So the incomplete push is the NEGATIVE CONTROL for the complete one, inside the
 * same capture, and a master that waits for a `04 03` JOIN from a warm box waits
 * for a frame the box does not send first.
 *
 * WHAT THIS FILE PINS. Three things, none of them a re-derivation of the desk:
 *
 *  1. THE BURST GEOMETRY IS THE DESK'S, AT EVERY RATE. HEAD -> LAST spans 2511
 *     slots at 44.1 kHz — the counter delta measured above. `fps / 500` TRUNCATES
 *     7.35 to 7 there and put the whole transfer out in 2392 slots, 4.7 % fast;
 *     48 kHz (8.0) and 96 kHz (16.0) divide exactly and never showed it. The
 *     rounded-rational burst slot reproduces 2511 exactly and leaves 48/96 kHz
 *     byte-identical, which is why both are asserted here.
 *
 *  2. EVERY TRANSFER THIS MASTER EMITS IS COMPLETE — FIRST, 341 MIDDLEs, LAST, in
 *     that order, from the very first slot after IDLE. An interrupted push is
 *     measured to produce NOTHING, so a LAST without its FIRST is not a slower
 *     courtship, it is no courtship at all.
 *
 *  3. A LINKED SILENT BOX IS CAPTURED WITH NO CABLE TOUCHED. The fake box below
 *     behaves the way the capture's box behaves and no more generously: it is
 *     silent until a COMPLETE transfer reaches it, it answers with the capture's
 *     own commit-report block, and it NEVER cold-connects on its own. The master
 *     must reach ESTABLISHED off that alone, having received no `04 03` JOIN.
 *
 * Pure: no socket, no thread, no rig. The bytes are the capture's.
 */

#include <reac/reac.h>
#include <reac/reac_master.h>
#include <reac/reac_ctrl.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_ports.h>

#include <stdio.h>
#include <string.h>

#define CHK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL: %s (line %d)\n", #x, __LINE__); return 1; } } while (0)

/* Our own NIC's address — a master emits from the L2 source, never a cloned desk
 * MAC (reac_mac.h). */
static const uint8_t OUR[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };
/* The S-4000S-3208 of the capture. */
static const uint8_t BOX[6] = { 0x00, 0x40, 0xab, 0xc4, 0x08, 0xbc };

/* The box's state-4 COMMIT REPORT, verbatim from desk-arrival-slice.pcap at
 * t=1789372394.229741 — `cdea 01 03 0010 84`, port table `02 x8, 01 x2, 03 x2`
 * (32 in / 8 out) and chassis strap block[7] = 0x00. This is the whole frame the
 * warm box answers a completed push with; there is no `04 03` before it. */
static const uint8_t COMMIT_REPORT[32] = {
	0x01, 0x03, 0x00, 0x10, 0x84, 0x00, 0x00, 0x00,
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
	0x01, 0x01, 0x03, 0x03, 0x00, 0x03, 0x00, 0x00,
	0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c,
};

/* The box's `cdea 04 03` burst, verbatim from the same capture at
 * t=1789372395.927676/.927926/.928265 — the join record, the head mark and the
 * box-ready record. It arrives 1.698 s AFTER the commit report and only once the
 * ENROLL group map has gone out, which is the whole point: a master that waits
 * for this to start courting waits behind its own grant. */
static const uint8_t JOIN_BURST[3][32] = {
	{ 0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe,
	  0x0f, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
	  0x01, 0x00, 0x06, 0x00, 0x03, 0x00, 0x76, 0xf7,
	  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe,
	  0x0f, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
	  0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x7d, 0xf7,
	  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe,
	  0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
	  0x03, 0x02, 0x00, 0x01, 0x00, 0x7a, 0xf7, 0x00,
	  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 },
};

/* The FIRST and LAST blocks the desk put on the wire for that transfer, read off
 * the same capture (block[0:16] — link, segment, declared payload length, opcode,
 * then the declared total 0x22c8 and the body's first bytes / the LAST frame's
 * fixed trailer). Compared against ours so "our push is the desk's push" is a
 * byte claim, not a paraphrase. */
static const uint8_t DESK_FIRST16[16] = {
	0x01, 0x01, 0x00, 0x18, 0x00, 0x22, 0xc8, 0x31,
	0x32, 0x33, 0x34, 0x01, 0x00, 0x00, 0x00, 0x04,
};
static const uint8_t DESK_LAST16[16] = {
	0x01, 0x02, 0x00, 0x0e, 0x00, 0x03, 0x00, 0x00,
	0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* HEAD -> LAST, in slots, per rate. 44.1 kHz is the capture's counter delta
 * (49350 - 46839); 48 and 96 kHz are the exact `fps/500` the M-300 and M-200
 * goldens were read at (8 and 16 slots per chunk), and they are the control that
 * the rational stride changed nothing where the divisor already divided. */
static const struct { int fps; int head_to_last; } BURST[] = {
	{ 3675, 2511 },   /* 44.1 kHz — desk-arrival-q4-2026-09-14, MEASURED       */
	{ 4000, 2733 },   /* 48   kHz — 5 + 340*8  + 8                             */
	{ 8000, 5461 },   /* 96   kHz — 5 + 340*16 + 16                            */
};

/* Build the box->master frame the capture shows: unicast to us, the box's own
 * Roland-OUI source, one 32-byte control block, an 8-channel upstream body (the
 * width the box returns at before the ENROLL group map widens it). */
static size_t box_frame(uint8_t *out, const uint8_t blk[32], int n_ch)
{
	size_t len = reac_ctrl_box_frame_len(n_ch);
	memset(out, 0, len);
	memcpy(out, OUR, 6);
	memcpy(out + 6, BOX, 6);
	out[12] = 0x88; out[13] = 0x19;
	out[16] = 0xcd; out[17] = 0xea;
	memcpy(out + 18, blk, 32);
	out[len - 2] = REAC_END_MARKER_0;
	out[len - 1] = REAC_END_MARKER_1;
	reac_ctrl_checksum_apply(out);
	return len;
}

/* ---- arm 1 + 2: the burst geometry, and that every transfer completes ------ */

static int burst_geometry(int fps, int want_head_to_last)
{
	struct reac_console_cfg cfg = { .out_channels = 8,
	                                .console_field = reac_pace_code(fps) };
	struct reac_master m;
	reac_master_init(&m, OUR, &cfg, fps);

	long head_slot = -1, last_slot = -1, last_chunk_slot = -1;
	int mids = 0, transfers = 0, orphan_last = 0, orphan_first = 0, inflight = 0;
	int first_emit_was_head = -1;

	/* Three full cycles: the cycle is 10778 slots at 4000 fps and scales. */
	for (long slot = 0; slot < (long)fps * 9; slot++) {
		uint16_t ctr; int idx;
		enum reac_master_emit e = reac_master_next(&m, &ctr, &idx);

		if (e == REAC_M_EMIT_SCENE_HEAD) {
			if (first_emit_was_head < 0)
				first_emit_was_head = (slot == 0);
			if (inflight)
				orphan_first++;      /* a FIRST cancelling a live transfer */
			inflight = 1; mids = 0; head_slot = slot;
			/* The header declares the total and opens the body. */
			CHK(memcmp(m.scene_blk + 2, DESK_FIRST16, 16) == 0);
		} else if (e == REAC_M_EMIT_SCENE_CHUNK) {
			if (inflight)
				mids++;
			/* THE CHUNK INDEX IS COMPUTED FROM THE SLOT, NOT COUNTED. It has to
			 * be: a cursor would be a field, and a field in this public struct
			 * moves every member behind it (rig, 2026-09-14). So the steps are
			 * checked to arrive 1..341 IN ORDER, which is the one thing a
			 * stateless inverse of the burst slot can get wrong. */
			CHK(m.scene_step == mids);
			last_chunk_slot = slot;
		} else if (e == REAC_M_EMIT_SCENE_TAIL) {
			if (!inflight) { orphan_last++; continue; }
			last_slot = slot;
			CHK(memcmp(m.scene_blk + 2, DESK_LAST16, 16) == 0);
			CHK(mids == REAC_SCENE_CHUNKS);
			CHK(last_slot - head_slot == want_head_to_last);
			inflight = 0; transfers++;
		} else if (first_emit_was_head < 0) {
			first_emit_was_head = 0;     /* something else opened the wire */
		}
	}

	/* The first frame this master ever emits is the FIRST of a transfer — a
	 * courtship that opens with MIDDLEs is the capture's negative control. */
	CHK(first_emit_was_head == 1);
	CHK(orphan_last == 0);
	CHK(orphan_first == 0);
	CHK(transfers >= 3);

	/* The burst ends where burst_end says it does — the emitted sequence and the
	 * struct's own arithmetic are the same fact, checked against each other
	 * rather than each against itself. The header goes out 5 slots before the
	 * cycle opens, so chunk 341 lands at head + 5 + burst_end. */
	CHK(last_chunk_slot - head_slot == 5 + m.burst_end);
	return 0;
}

/* ---- arm 3: the warm box, captured with no cable touched ------------------- */

/* +8.4 ms, the measured gap between the desk's LAST and the box's first frame. */
#define COMMIT_DELAY_MS   8.4
/* +1.489 s, the measured gap between the desk's group map and the box's burst. */
#define JOIN_DELAY_S      1.489

static int warm_relink(int fps)
{
	struct reac_console_cfg cfg = { .out_channels = 8,
	                                .console_field = reac_pace_code(fps) };
	struct reac_master m;
	reac_master_init(&m, OUR, &cfg, fps);

	/* The fake box, and it is no more generous than the capture's: silent until a
	 * COMPLETE transfer arrives, answers that with the commit report, and never
	 * cold-connects on its own. `saw_first` is the whole of its rule. */
	int saw_first = 0, mids = 0, committed = 0, group_map_seen = 0;
	long commit_at = -1;
	int joins_sent = 0;
	long established_at = -1, commit_sent_at = -1, group_map_at = -1;

	/* The box's upstream return, built once: unicast to us, 8 channels wide,
	 * silent audio. Its ARRIVAL is what a linked box proves with. */
	uint8_t upstream[REAC_FRAME_BYTES];
	size_t upstream_len = reac_ctrl_build_upstream_filler(upstream, OUR, BOX, 0, 8,
	                                                      NULL, REAC_SAMPLES_PER_PKT);
	if (upstream_len == 0) { fprintf(stderr, "FAIL: upstream filler\n"); return 1; }

	for (long slot = 0; slot < (long)fps * 12; slot++) {
		uint16_t ctr; int idx;
		enum reac_master_emit e = reac_master_next(&m, &ctr, &idx);

		switch (e) {
		case REAC_M_EMIT_SCENE_HEAD:
			saw_first = 1; mids = 0;
			break;
		case REAC_M_EMIT_SCENE_CHUNK:
			if (saw_first) mids++;
			break;
		case REAC_M_EMIT_SCENE_TAIL:
			/* THE RULE THE CAPTURE MEASURES: only a complete transfer commits. */
			if (saw_first && mids == REAC_SCENE_CHUNKS && !committed)
				commit_at = slot + (long)(COMMIT_DELAY_MS * fps / 1000.0);
			saw_first = 0; mids = 0;
			break;
		case REAC_M_EMIT_ENROLL:
			if (committed && !group_map_seen) {
				group_map_seen = 1;
				group_map_at = slot;
			}
			break;
		default:
			break;
		}

		if (commit_at >= 0 && slot == commit_at) {
			uint8_t f[REAC_FRAME_BYTES];
			size_t n = box_frame(f, COMMIT_REPORT, 8);
			struct reac_ctrl_parsed p;
			enum reac_master_rx_event ev;

			/* The classifier must call it a CONFIG announce — it is the box's
			 * setup declaration, arriving with no `04 03` in front of it. */
			CHK(reac_ctrl_classify_box_frame(f, n, OUR, &p, &ev) == 0);
			CHK(ev == REAC_M_RX_BOX_CONFIG);

			/* The geometry comes out of the declaration, never a model list. */
			struct reac_box_ports ports;
			CHK(reac_ports_parse(f + REAC_CTRL_BLOCK_OFF, &ports) == 0);
			CHK(ports.in_ch == 32 && ports.out_ch == 8);
			CHK(ports.headamp_base == 0x00);
			reac_master_set_box(&m, ports.in_ch, ports.out_ch, ports.headamp_base);
			CHK(reac_master_has_box(&m));

			reac_master_rx(&m, ev, p.src, NULL);
			/* PROBING leaves on the config announce — no JOIN was ever sent. */
			CHK(m.state == REAC_M_GRANTING);
			committed = 1; commit_sent_at = slot; commit_at = -1;
		}

		/* The box's `04 03` burst comes 1.489 s AFTER the group map, so it is
		 * never what OPENS the courtship — by the time it lands the master has
		 * already granted this box. Replayed verbatim so the model stays the
		 * capture's, and so the late burst is pinned as a no-op rather than a
		 * second courtship: a re-latch here would restart the dwell and cut the
		 * sweep the box is holding. */
		if (group_map_seen && !joins_sent &&
		    slot == group_map_at + (long)(JOIN_DELAY_S * fps)) {
			enum reac_master_state before = m.state;
			for (size_t j = 0; j < sizeof JOIN_BURST / sizeof JOIN_BURST[0]; j++) {
				uint8_t f[REAC_FRAME_BYTES];
				size_t n = box_frame(f, JOIN_BURST[j], 8);
				struct reac_ctrl_parsed p;
				enum reac_master_rx_event ev;
				if (reac_ctrl_classify_box_frame(f, n, OUR, &p, &ev) != 0)
					continue;   /* a link-4 record with no FSM action */
				reac_master_rx(&m, ev, p.src,
				               ev == REAC_M_RX_BOX_JOIN ? f + REAC_CTRL_BLOCK_OFF : NULL);
			}
			CHK(m.state == before);
			joins_sent = 1;
		}

		/* ONCE IT HAS COMMITTED, THE BOX STREAMS. The capture shows 774 frames of
		 * 340-byte (8-channel) upstream in the 210 ms between its commit report
		 * and the group map, and full-width audio after it — so a warm box is
		 * never silent again from here, and the master's link-check budget is
		 * reloaded by its frames rather than draining under a fake that stopped
		 * talking. */
		if (committed) {
			struct reac_ctrl_parsed p;
			enum reac_master_rx_event ev;
			if (reac_ctrl_classify_box_frame(upstream, upstream_len, OUR,
			                                 &p, &ev) == 0)
				reac_master_rx(&m, ev, p.src, NULL);
		}

		if (m.state == REAC_M_ESTABLISHED && established_at < 0)
			established_at = slot;
	}

	/* Captured: no cable touched, no cold connect, no JOIN. */
	CHK(committed);
	CHK(group_map_seen);
	/* The group map follows the report inside the desk's measured ~210 ms. */
	CHK(group_map_at - commit_sent_at >= 0);
	CHK(group_map_at - commit_sent_at <= (long)(0.210 * fps));
	CHK(established_at > 0);
	/* And the box the master ended up holding is the one that declared itself. */
	CHK(memcmp(m.box_mac, BOX, 6) == 0);
	CHK(m.alloc.width == 32);
	return 0;
}

int main(void)
{
	for (size_t i = 0; i < sizeof BURST / sizeof BURST[0]; i++)
		if (burst_geometry(BURST[i].fps, BURST[i].head_to_last)) {
			fprintf(stderr, "  (at %d fps)\n", BURST[i].fps);
			return 1;
		}

	for (size_t i = 0; i < sizeof BURST / sizeof BURST[0]; i++)
		if (warm_relink(BURST[i].fps)) {
			fprintf(stderr, "  (at %d fps)\n", BURST[i].fps);
			return 1;
		}

	printf("OK: every transfer this master emits is COMPLETE (FIRST, 341 MIDDLEs, "
	       "LAST) from its first slot, spanning the desk's own 2511/2733/5461 slots "
	       "at 44.1/48/96 kHz with the capture's FIRST and LAST blocks byte for byte; "
	       "and a linked, silent S-4000S that answers only a completed push — with the "
	       "state-4 commit report, never a cold connect — is recognized at 32 in / 8 "
	       "out, granted and ESTABLISHED, with no cable touched and no 04 03 JOIN\n");
	return 0;
}
