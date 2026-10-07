// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* A DESK IS TOLD FROM A BOX BY DIRECTION, SOURCE AND ROLE — NEVER BY WIDTH.
 *
 * Operator ruling 2026-09-25: "BOX_MAX_CHANNELS = 40. We are dealing with a
 * S-4000S-3208 (32 in, 8 out), we also have S-2416 (24 in, 16 out), and we tested
 * an 8 in / 32 out box." A box may be 40 wide, so the old length rule (40 -> DESK,
 * narrower -> BOX, reac_rival_kind_from_channels) cannot decide who a peer is. This
 * was libreac review 2026-09-25 finding M4.
 *
 * Both ways, through the real doors (reac_disco_classify -> the discovery table ->
 * reac_rival_kind_of / reac_arbitrate):
 *   BOXES  the three known models — 32/8, 24/16, 8/32 — and a 40-in box, each
 *          declaring itself (config announce) and returning audio UNICAST to its
 *          master: every one is a BOX, the 40-wide one included, and none is a
 *          foreign master on the wire;
 *   DESK   a 40-wide BROADCAST downstream with its cfea master announce is the
 *          DESK, and arbitration reports it as the foreign master to join;
 *   BOX ON M a box that declares its model and then announces master (a stagebox
 *          strapped to master, §2b) is refused as a BOX, whatever its width. */
#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_disco.h>
#include <reac/reac_arbitration.h>
#include <reac/reac_hunt.h>
#include <reac/reac_master.h>
#include <reac/reac_encode.h>
#include <reac/reac_upstream.h>

#include <stdio.h>
#include <string.h>

#include "ctrl_fixtures.inc"   /* FX_ANNOUNCE — a captured cfea master announce */

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static const uint8_t US[6]    = { 0x00, 0x40, 0xab, 0x77, 0x77, 0x77 };   /* this daemon */
static const uint8_t DESK[6]  = { 0x00, 0x40, 0xab, 0xc9, 0x91, 0x9c };
static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static uint8_t frame[2048];

/* Classify one frame and fold it into the table, the way the pacer does. */
static int see(struct reac_disco_table *t, const uint8_t *f, size_t len, uint64_t now)
{
	struct reac_disco_sighting s;
	if (reac_disco_classify(f, len, US, &s) != 0)
		return -1;
	reac_disco_table_observe(t, &s, 0, now);
	return 0;
}

static const struct reac_disco_entry *entry(const struct reac_disco_table *t,
                                            const uint8_t mac[6])
{
	for (int i = 0; i < t->n; i++)
		if (memcmp(t->e[i].mac, mac, 6) == 0)
			return &t->e[i];
	return NULL;
}

/* The desk's own downstream frame: 40 wide, BROADCAST, from the desk. */
static size_t desk_downstream(uint8_t *f)
{
	float *none[1] = { NULL };
	return (size_t)reac_downstream_build(f, none, 0, 12, 7, DESK);
}

/* The same frame with the desk's cfea master announce over its control block. */
static size_t desk_announce(uint8_t *f)
{
	size_t len = desk_downstream(f);
	memcpy(f + 16, FX_ANNOUNCE, sizeof FX_ANNOUNCE);
	return len;
}

/* One box, by model token: it declares itself and returns its audio to US. */
static int box_is_a_box(const char *token, int want_width)
{
	const int before = fails;
	const struct reac_box_model *m = reac_box_catalogue_by_token(token);
	CHK(m != NULL);
	if (!m)
		return 0;
	uint8_t mac[6] = { 0x00, 0x40, 0xab, 0x10, 0x20, (uint8_t)m->in_ch };
	const int w = reac_box_model_upstream_width(m);
	CHK(w == want_width);

	struct reac_disco_table t;
	reac_disco_table_init(&t);
	size_t l1 = reac_ctrl_build_as(frame, m, REAC_BOX_BLOCK_CONFIG, US, mac, 1, NULL, 0);
	CHK(l1 > 0 && see(&t, frame, l1, 1000) == 0);
	size_t l2 = reac_ctrl_build_upstream_filler(frame, US, mac, 2, w, NULL, 0);
	CHK(l2 == reac_ctrl_box_frame_len(w) && see(&t, frame, l2, 2000) == 0);
	CHK(reac_upstream_channels(l2) == w);

	const struct reac_disco_entry *e = entry(&t, mac);
	CHK(e && e->role == REAC_DISCO_ROLE_BOX && e->has_decl &&
	    e->decl_in == m->in_ch && e->decl_out == m->out_ch);
	CHK(e && e->channels == (unsigned)w);
	CHK(reac_rival_kind_of(e) == REAC_RIVAL_BOX);

	struct reac_arbitration a;
	reac_arbitrate(&t, US, REAC_M_IDLE, REAC_PACE_FREE_RUN, 3000, &a);
	CHK(a.state != REAC_SEGMENT_FOREIGN && a.rival == REAC_RIVAL_NONE);
	return fails == before;
}

int main(void)
{
	/* ---- BOXES: the three known models, and a box sending 40 upstream ---- */
	CHK(box_is_a_box("s4000s", 32));        /* S-4000S-3208: 32 in / 8 out */
	CHK(box_is_a_box("s2416", 24));         /* S-2416:       24 in / 16 out */
	CHK(box_is_a_box("s4000s-0832", 8));    /* the 8 in / 32 out box tested */
	CHK(box_is_a_box("fr4000", 40));        /* 40 upstream, 1492 B: still a box */

	/* ---- DESK: the 40-wide broadcast downstream that announces master ---- */
	{
		struct reac_disco_table t;
		reac_disco_table_init(&t);
		size_t l = desk_downstream(frame);
		CHK(l == REAC_FRAME_BYTES && memcmp(frame, BCAST, 6) == 0);
		CHK(see(&t, frame, l, 1000) == 0);
		CHK(see(&t, frame, desk_announce(frame), 2000) == 0);
		const struct reac_disco_entry *e = entry(&t, DESK);
		CHK(e && e->role == REAC_DISCO_ROLE_MASTER && !e->has_decl);
		CHK(e && e->channels == REAC_MAX_CHANNELS);
		CHK(reac_rival_kind_of(e) == REAC_RIVAL_DESK);

		struct reac_arbitration a;
		reac_arbitrate(&t, US, REAC_M_IDLE, REAC_PACE_FREE_RUN, 3000, &a);
		CHK(a.state == REAC_SEGMENT_FOREIGN && a.rival == REAC_RIVAL_DESK);
		CHK(a.have_mac && memcmp(a.mac, DESK, 6) == 0);
		CHK(strcmp(reac_rival_refusal(a.rival), "none") == 0);   /* a desk is joined */
	}

	/* ---- BOX ON M: it declared a box model, then announced master ---- */
	{
		const struct reac_box_model *m = reac_box_catalogue_by_token("s2416");
		static const uint8_t BOXM[6] = { 0x00, 0x40, 0xab, 0x24, 0x16, 0x01 };
		struct reac_disco_table t;
		reac_disco_table_init(&t);
		size_t l = reac_ctrl_build_as(frame, m, REAC_BOX_BLOCK_CONFIG, BCAST, BOXM, 1, NULL, 0);
		CHK(see(&t, frame, l, 1000) == 0);
		/* its master-mode broadcast, 40 wide, with a master announce */
		size_t ld = desk_announce(frame);
		memcpy(frame + 6, BOXM, 6);
		CHK(see(&t, frame, ld, 2000) == 0);
		const struct reac_disco_entry *e = entry(&t, BOXM);
		CHK(e && e->has_decl && e->decl_in == m->in_ch && e->decl_out == m->out_ch);
		CHK(reac_rival_kind_of(e) == REAC_RIVAL_BOX);
		CHK(strcmp(reac_rival_refusal(reac_rival_kind_of(e)), "rival-master-box") == 0);
	}

	/* ---- DESK OR BOX MASTER, FROM THE WIRE (ruling 2026-10-07) ----
	 * A box on M sends cfea, chanmap and scene pushes like a desk; what tells them apart
	 * is the cfea's total_slots (block[15]) and the broadcast's width. Captured blocks:
	 * the S-1608 on M (box-to-box-2026-09-13 enrol-main-port-slice.pcap frame 3544, a
	 * 628 B broadcast) and the M-200 (real-m200-s1608-coldboot-2026-07-11 frame 754). */
	{
		static const char *S1608_ON_M =
			"cfeaffff010001030d01040040abc4803b1008010001000000000000000000000067";
		static const char *M200 =
			"cfeaffff010001030d01040040abc9cc03281000000100000000000000000000002f";
		static const uint8_t BOXM[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x3b };
		static const uint8_t M200M[6] = { 0x00, 0x40, 0xab, 0xc9, 0xcc, 0x03 };
		struct reac_disco_table t;
		struct reac_arbitration a;
		const struct reac_disco_entry *e;
		uint8_t win[34];
		for (int i = 0; i < 34; i++) { unsigned v; sscanf(S1608_ON_M + 2 * i, "%2x", &v); win[i] = (uint8_t)v; }

		/* 1. the S-1608 on M: 628 B broadcast, cfea 0x10 -> a BOX, 16 wide */
		reac_disco_table_init(&t);
		size_t l = reac_ctrl_build_flood_filler(frame, BCAST, BOXM, 1, 16, NULL, 12);
		CHK(l == 628);
		memcpy(frame + 16, win, 34);
		CHK(see(&t, frame, l, 1000) == 0);
		e = entry(&t, BOXM);
		CHK(e && e->role == REAC_DISCO_ROLE_MASTER && e->announced_slots == 0x10);
		CHK(reac_rival_kind_of(e) == REAC_RIVAL_BOX);
		reac_arbitrate(&t, US, REAC_M_IDLE, REAC_PACE_FREE_RUN, 2000, &a);
		CHK(a.state == REAC_SEGMENT_FOREIGN && a.rival == REAC_RIVAL_BOX && a.rival_channels == 16);

		/* 2. the same box's cfea in a 1492 B broadcast: still a BOX — the announce
		 * says 16, whatever the frame says */
		reac_disco_table_init(&t);
		l = desk_downstream(frame);
		memcpy(frame + 6, BOXM, 6);
		memcpy(frame + 16, win, 34);
		CHK(see(&t, frame, l, 1000) == 0);
		CHK(reac_rival_kind_of(entry(&t, BOXM)) == REAC_RIVAL_BOX);

		/* 3. the M-200: 1492 B broadcast, cfea 0x28 -> a DESK */
		for (int i = 0; i < 34; i++) { unsigned v; sscanf(M200 + 2 * i, "%2x", &v); win[i] = (uint8_t)v; }
		reac_disco_table_init(&t);
		l = desk_downstream(frame);
		memcpy(frame + 6, M200M, 6);
		memcpy(frame + 16, win, 34);
		CHK(see(&t, frame, l, 1000) == 0);
		e = entry(&t, M200M);
		CHK(e && e->announced_slots == 0x28 && e->channels == REAC_MAX_CHANNELS);
		CHK(reac_rival_kind_of(e) == REAC_RIVAL_DESK);

		/* 4. a 1492 B master with no cfea heard yet (a chanmap only): PENDING, never a
		 * desk by default — and the hunt waits rather than join or refuse */
		reac_disco_table_init(&t);
		struct reac_disco_entry pend = { .role = REAC_DISCO_ROLE_MASTER,
		                                 .channels = REAC_MAX_CHANNELS,
		                                 .first_seen_ns = 1000, .last_seen_ns = 1000 };
		memcpy(pend.mac, M200M, 6);
		t.e[t.n++] = pend;
		CHK(reac_rival_kind_of(&t.e[0]) == REAC_RIVAL_PENDING);
		reac_arbitrate(&t, US, REAC_M_IDLE, REAC_PACE_FREE_RUN, 2000, &a);
		CHK(a.state == REAC_SEGMENT_FOREIGN && a.rival == REAC_RIVAL_PENDING);
		CHK(strcmp(reac_rival_kind_name(REAC_RIVAL_PENDING), "pending") == 0);
		CHK(strcmp(reac_rival_refusal(REAC_RIVAL_PENDING), "none") == 0);

		/* 4b. ...end to end through the hunt: a 1492 B chanmap from a master that has
		 * not announced itself leaves the wire HUNTING, then its cfea 0x28 joins it */
		{
			struct reac_hunt h;
			reac_hunt_init(&h, US, 1000);
			static struct reac_master m;
			reac_master_init(&m, M200M, NULL, 4000);
			size_t hl = desk_downstream(frame);
			memcpy(frame + 6, M200M, 6);
			CHK(reac_master_stamp(&m, frame, REAC_M_EMIT_CHANMAP, 0) == 0);
			CHK(reac_hunt_observe(&h, frame, hl, 2000, NULL) >= 0);
			reac_hunt_step(&h, 3000);
			CHK(h.arb.rival == REAC_RIVAL_PENDING);
			CHK(h.verdict == REAC_HUNT_HUNTING);
			l = desk_downstream(frame);
			memcpy(frame + 6, M200M, 6);
			memcpy(frame + 16, win, 34);                /* the M-200's own cfea */
			CHK(reac_hunt_observe(&h, frame, l, 4000, NULL) >= 0);
			reac_hunt_step(&h, 5000);
			CHK(h.arb.rival == REAC_RIVAL_DESK && h.verdict == REAC_HUNT_SLAVE);
		}
		/* 4b'. pending changes nothing: a wire already decided keeps its verdict */
		{
			struct reac_hunt h;
			reac_hunt_init(&h, US, 1000);
			h.verdict = REAC_HUNT_MASTER;              /* we were driving it */
			static struct reac_master m2;
			reac_master_init(&m2, M200M, NULL, 4000);
			size_t hl = desk_downstream(frame);
			memcpy(frame + 6, M200M, 6);
			CHK(reac_master_stamp(&m2, frame, REAC_M_EMIT_CHANMAP, 0) == 0);
			CHK(reac_hunt_observe(&h, frame, hl, 2000, NULL) >= 0);
			reac_hunt_step(&h, 3000);
			CHK(h.arb.rival == REAC_RIVAL_PENDING);
			CHK(h.verdict == REAC_HUNT_MASTER);
		}

		/* 4c. the cfea crosses the pacer's ring: a gate that let only role and
		 * declaration through would hold a running segment pending for ever */
		{
			struct reac_disco_gate g;
			reac_disco_gate_init(&g);
			struct reac_disco_sighting sg = { .role = REAC_DISCO_ROLE_MASTER,
			                                  .channels = REAC_MAX_CHANNELS };
			memcpy(sg.mac, M200M, 6);
			CHK(reac_disco_gate_should_push(&g, &sg, 1000) == 1);
			CHK(reac_disco_gate_should_push(&g, &sg, 2000) == 0);   /* a repeat */
			sg.announced_slots = 0x28;
			CHK(reac_disco_gate_should_push(&g, &sg, 3000) == 1);   /* its announce */
			CHK(reac_disco_gate_should_push(&g, &sg, 4000) == 0);
		}

		/* 5. a box-width broadcast master with no cfea (a box on M that never
		 * announces): a BOX by its width */
		t.e[0].channels = 8;
		CHK(reac_rival_kind_of(&t.e[0]) == REAC_RIVAL_BOX);

		/* 6. THE KNOWN GAP, written down: a 40-input box on M announcing 0x28 in a
		 * 1492 B broadcast reads as a DESK until one is captured */
		t.e[0].channels = REAC_MAX_CHANNELS;
		t.e[0].announced_slots = 0x28;
		CHK(reac_rival_kind_of(&t.e[0]) == REAC_RIVAL_DESK);
	}

	/* ---- the width alone decides nothing: the old rule would have said DESK for
	 * the 40-wide box above and BOX for nothing a 40-wide desk sent ---- */
	CHK(reac_rival_kind_from_channels(40) == REAC_RIVAL_DESK);   /* documents the old rule */

	if (fails) {
		fprintf(stderr, "%d desk-or-box check(s) failed\n", fails);
		return 1;
	}
	printf("OK: test_desk_or_box — S-4000S-3208 (32/8), S-2416 (24/16), the 8/32 box and a "
	       "40-in box returning 40 upstream are all BOXES; the 40-wide broadcast downstream "
	       "that announces master is the DESK; a box that declared its model and then "
	       "announced master is refused as a box — by direction and role, never width\n");
	return 0;
}
