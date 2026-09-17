// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* THE LIVE DEFECT OF 2026-09-17, AS THE WIRE PRODUCED IT.
 *
 * An 8-in / 32-out split (chassis label S-4000H; a real M-200 displays it as an
 * S-4000S) alone on VLAN 13 with this daemon mastering it. The operator saw two lines and then nothing for minutes:
 *
 *     REAC heard — box 00:40:ab:c4:25:80 (8 ch): this interface is a segment
 *     REAC heard — unknown 00:40:ab:c4:25:80 (32 ch): this interface is a segment
 *     roster: state=probing model=none role=master width=0/0
 *
 * Three defects, one box, and this file is the red for each of them — every arm
 * fails on the code as it shipped in 1.0.14 / libreac 1.2.0:
 *
 *   ARM 1  the box's own declaration decodes to 8 in / 32 out. It did not: the
 *          port table's 0x00 groups made reac_ports_parse refuse the whole
 *          table, so reac_master_set_box was never called at all.
 *   ARM 2  the declaration NAMES the box, byte for byte, as the model table's
 *          s4000s-0832 row — which was a DERIVED row with no bytes in it, and is
 *          now captured from this wire and from an M-200 power-cycle.
 *   ARM 3  the classifier answers ONE verdict for one MAC across every frame
 *          the box sent. It answered per frame: box/8 then unknown/32.
 *   ARM 4  the master reaches ESTABLISHED at the DECLARED width and stays
 *          there while the box streams its 32-channel return. It fell out of
 *          GRANTING into PROBING with box-undeclared, forever.
 *
 * The frames are the capture's own bytes (tests/box_0832_fixtures.inc). A
 * PROBE THAT CANNOT REPORT PRESENCE VOIDS EVERY ABSENCE IT REPORTS, so arm 3
 * asserts it classified every frame before it asserts there is only one peer.
 */
#include <reac/reac_ctrl.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_disco.h>
#include <reac/reac_hunt.h>
#include <reac/reac_link_state.h>
#include <reac/reac_master.h>
#include <reac/reac_ports.h>
#include <reac/reac_upstream.h>
#include <reac/reac.h>

#include <stdio.h>
#include <string.h>

#include "box_0832_fixtures.inc"

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* The wire frame, rebuilt from a fixture row: the 50 captured bytes, silent
 * audio, and the C2 EA end marker the capture carried. */
static size_t frame_0832(const struct box_0832_frame *f, uint8_t *out)
{
	memset(out, 0, f->len);
	memcpy(out, f->head, sizeof f->head);
	out[f->len - 2] = 0xc2;
	out[f->len - 1] = 0xea;
	return f->len;
}

/* The one config-announce in the file. */
static size_t config_announce(uint8_t *out)
{
	for (size_t i = 0; i < BOX_0832_N; i++) {
		const uint8_t *h = BOX_0832_FRAMES[i].head;
		if (h[16] == 0xcd && h[17] == 0xea && h[18] == 0x01 && h[22] == 0x84)
			return frame_0832(&BOX_0832_FRAMES[i], out);
	}
	return 0;
}

int main(void)
{
	uint8_t frame[2048];

	/* ---- ARM 1: the declaration is the geometry ---- */
	size_t clen = config_announce(frame);
	CHK(clen == 340);                     /* an 8-channel frame carried it */
	struct reac_box_ports ports;
	CHK(reac_ports_parse(frame + REAC_CTRL_BLOCK_OFF, &ports) == 0);
	CHK(ports.in_ch == 8);
	CHK(ports.out_ch == 32);
	CHK(ports.headamp_base == 0x00);      /* strap 0, as announced */
	CHK(reac_ports_unknown(frame + REAC_CTRL_BLOCK_OFF, NULL) == 0);
	/* THE FRAME LENGTH IS NOT THE GEOMETRY, and this box is why that matters:
	 * it declared 8 inputs in a 340 B frame and returns 1204 B ones. */
	CHK(reac_upstream_channels(1204) == 32);
	CHK(reac_upstream_channels(clen) == 8);

	/* ---- ARM 2: the declaration NAMES the box ---- */
	const struct reac_box_model *bm = reac_ctrl_identify_box(frame, clen);
	CHK(bm != NULL);
	CHK(strcmp(bm->token, "s4000s-0832") == 0);
	CHK(bm->in_ch == 8 && bm->out_ch == 32);
	CHK(bm->origin == REAC_BOX_CAPTURED);
	/* ITS GRANTED RETURN IS 8, measured on a real M-200 (217 905 frames of
	 * 340 B). The 1204 B frames below are what it floods UNGRANTED. */
	CHK(reac_box_model_upstream_width(bm) == 8);
	/* AND ITS IDENTITY PAGE IS THE S-4000S-3208's, byte for byte — one chassis,
	 * two straps, and the reason the M-200 displays this box as an S-4000S.
	 * Captured from the power-cycle, not copied: m200-s4000h-coldboot.pcap. */
	{
		const struct reac_box_model *s32 = reac_box_model_by_token("s4000s");
		uint8_t a[32], b[32];
		CHK(s32 && reac_box_model_block(bm, REAC_BOX_BLOCK_CC0016, a) == 1);
		CHK(reac_box_model_block(s32, REAC_BOX_BLOCK_CC0016, b) == 1);
		CHK(memcmp(a, b, 32) == 0);
		CHK(reac_box_model_block(bm, REAC_BOX_BLOCK_CC001A, a) == 1);
		CHK(reac_box_model_block(s32, REAC_BOX_BLOCK_CC001A, b) == 1);
		CHK(memcmp(a, b, 32) == 0);
		CHK(bm->fw_milli == 2500);
		CHK(bm->reac_major == 2 && bm->reac_minor == 1 && bm->reac_patch == 2);
	}
	/* AND A WIDTH STILL DOES NOT NAME THIS BOX. The slave path has nothing but a
	 * width to go on (a stagebox on M declares nothing), and 8 inputs is what the
	 * S-0808 is: that row keeps the number, this one is named by its declaration
	 * or not at all. Asserted BOTH ways so the row cannot quietly take it. */
	const struct reac_box_model *by_width = reac_box_master_model(8);
	CHK(by_width != NULL && strcmp(by_width->token, "s0808") == 0);
	CHK(by_width != bm);
	CHK(reac_box_model_by_channels(8) != bm);

	/* ---- ARM 3: one MAC, one verdict ---- */
	struct reac_hunt h;
	reac_hunt_init(&h, BOX_0832_OUR, 0);
	int sightings = 0, roles_box = 0;
	for (size_t i = 0; i < BOX_0832_N; i++) {
		struct reac_disco_sighting s;
		size_t len = frame_0832(&BOX_0832_FRAMES[i], frame);
		uint64_t now = 1000000ull * (uint64_t)(i + 1);
		if (reac_hunt_observe(&h, frame, len, now, &s) < 0)
			continue;
		sightings++;
		CHK(memcmp(s.mac, BOX_0832_MAC, 6) == 0);
		/* AFTER the declaration lands, every later frame — including the
		 * 32-channel broadcast flood that read `unknown` — answers BOX. */
		if (i > 1) {
			CHK(s.role == REAC_DISCO_ROLE_BOX);
			CHK(s.model == bm);
			roles_box++;
		}
		if (s.role == REAC_DISCO_ROLE_BOX && i <= 1)
			roles_box++;
	}
	CHK(sightings == (int)BOX_0832_N);    /* presence, before any absence */
	CHK(roles_box >= (int)BOX_0832_N - 1);
	CHK(h.table.n == 1);                  /* ONE peer, not two verdicts */
	CHK(h.table.e[0].role == REAC_DISCO_ROLE_BOX);
	CHK(h.table.e[0].model == bm);
	CHK(h.table.e[0].channels == 32);     /* widest heard, beside 8 declared */

	/* ---- ARM 4: the grant reaches ESTABLISHED and SUSTAINS ---- */
	const int fps = 8000;
	struct reac_console_cfg cfg = { .out_channels = 8,
	                                .console_field = reac_pace_code(fps) };
	struct reac_master m;
	reac_master_init(&m, BOX_0832_OUR, &cfg, fps);

	long established_at = -1;
	int declared = 0;
	for (long slot = 0; slot < (long)fps * 6; slot++) {
		uint16_t ctr; int idx;
		reac_master_next(&m, &ctr, &idx);

		/* The box repeats its declaration until it is granted, ~1.5 s apart in
		 * the capture; one is enough to size it. */
		if (!declared && slot == fps / 2) {
			struct reac_ctrl_parsed p;
			enum reac_master_rx_event ev;
			CHK(reac_ctrl_classify_box_frame(frame, config_announce(frame),
			                                 BOX_0832_OUR, &p, &ev) == 0);
			CHK(ev == REAC_M_RX_BOX_CONFIG);
			CHK(reac_ports_parse(frame + REAC_CTRL_BLOCK_OFF, &ports) == 0);
			reac_master_set_box(&m, ports.in_ch, ports.out_ch,
			                    ports.headamp_base);
			CHK(reac_master_has_box(&m));
			CHK(m.alloc.width == 8);      /* the DECLARED inputs, not 32 */
			CHK(m.alloc.base == 0x00);
			reac_master_rx(&m, ev, p.src, NULL);
			declared = 1;
		}

		/* The box's 32-channel upstream, from the slot it first unicast one:
		 * this is the accept that establishes, and afterwards it is what has
		 * to KEEP the link — a budget that drains here is the defect the
		 * operator saw as `probing` returning. */
		if (declared && (slot % 8) == 0) {
			struct reac_ctrl_parsed p;
			enum reac_master_rx_event ev;
			uint8_t up[2048];
			size_t ulen = 0;
			for (size_t i = 0; i < BOX_0832_N; i++)
				if (BOX_0832_FRAMES[i].len == 1204 &&
				    BOX_0832_FRAMES[i].head[0] != 0xff &&
				    BOX_0832_FRAMES[i].head[16] == 0x00) {
					ulen = frame_0832(&BOX_0832_FRAMES[i], up);
					break;
				}
			CHK(ulen == 1204);
			if (reac_ctrl_classify_box_frame(up, ulen, BOX_0832_OUR, &p, &ev) == 0)
				reac_master_rx(&m, ev, p.src, NULL);
		}

		if (m.state == REAC_M_ESTABLISHED && established_at < 0)
			established_at = slot;
		/* SUSTAINED means it never came back: once established, no later slot
		 * may read anything else. */
		if (established_at >= 0)
			CHK(m.state == REAC_M_ESTABLISHED);
	}
	CHK(established_at > 0);
	CHK(m.alloc.width == 8);
	CHK(m.cfg.out_channels == 8);         /* the cfea width byte is the box's */

	printf("OK: the 0832 split's own frames — declared 8/32 at strap 0, named "
	       "s4000s-0832 byte-exact (identity page = the S-4000S-3208's, fw 2.500 "
	       "REAC 2.102), ONE verdict across %d sightings, ESTABLISHED at slot %ld "
	       "and sustained for %ld slots while it returned its ungranted 32-channel "
	       "frames\n", sightings, established_at, (long)fps * 6 - established_at);
	return 0;
}
