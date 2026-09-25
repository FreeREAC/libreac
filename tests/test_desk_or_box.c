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
	const struct reac_box_model *m = reac_box_model_by_token(token);
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
	CHK(e && e->role == REAC_DISCO_ROLE_BOX && e->model == m);
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
		CHK(e && e->role == REAC_DISCO_ROLE_MASTER && e->model == NULL);
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
		const struct reac_box_model *m = reac_box_model_by_token("s2416");
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
		CHK(e && e->model == m);
		CHK(reac_rival_kind_of(e) == REAC_RIVAL_BOX);
		CHK(strcmp(reac_rival_refusal(reac_rival_kind_of(e)), "rival-master-box") == 0);
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
