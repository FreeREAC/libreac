// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* HOLD WITH A DECLARED LIMIT. Operator ruling 2026-09-25, on the one case width used to
 * settle and can no longer (a box may be 40 wide): a BROADCAST audio stream whose source
 * has sent nothing that names its role is the desk's downstream or a box's
 * presence-flood. Hold the verdict until the sender's own frames prove its role, but
 * never longer than the SHORTEST master-only control cadence, in FRAMES at the current
 * rate; at that window's end a broadcast sender that sent no master-only op is a BOX.
 *
 * The cadence is reac-protocol's master_cadence group (MASTER_ONLY_CADENCE_FRAMES_44K1 /
 * _48K / _96K: the cfea announce, every 4000 frames of a 48 kHz downstream whether or not
 * a box answers — shorter than the page 0x0019 window and the scene repeat), read through
 * the generated reac_facts_master_cadence.h. The fact is FRAMES per rate; a duration is
 * only ever those frames at a pace (operator ruling 2026-09-25). Driven through the real doors — reac_hunt_observe
 * / reac_hunt_step and reac_sender_kind — on the hunt's own clock:
 *
 *   1. the window is the fact, in frames, at each pace — and one second at every pace;
 *   2. A DESK WHOSE MASTER OP ARRIVES INSIDE THE WINDOW IS THE DESK: held (neither driven
 *      nor joined) before it, joined on it;
 *   3. A FLOOD-ONLY BOX IS A BOX AFTER THE WINDOW, at 16 wide and at 40 wide: held inside
 *      it, a box from its end, and driven once the hunt's own vacancy window closes;
 *   4. the limit is the limit: a sender whose master op comes only AFTER the window was a
 *      box meanwhile, and that op still makes it the desk. */
#include <reac/reac.h>
#include <reac/reac_cfg.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_encode.h>
#include <reac/reac_hunt.h>
#include <reac/reac_arbitration.h>
#include <reac/reac_facts_master_cadence.h>

#include <stdio.h>
#include <string.h>

#include "ctrl_fixtures.inc"   /* FX_ANNOUNCE — a captured cfea master announce */

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define MS 1000000ULL
static const uint64_t T0 = 1000 * MS;
static const uint8_t US[6]    = { 0x00, 0x40, 0xab, 0x77, 0x77, 0x77 };
static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static uint8_t frame[2048];

static size_t desk_audio(const uint8_t mac[6], uint16_t ctr)
{
	float *none[1] = { NULL };
	return (size_t)reac_downstream_build(frame, none, 0, 12, ctr, mac);
}

static size_t desk_announce(const uint8_t mac[6], uint16_t ctr)
{
	size_t len = desk_audio(mac, ctr);
	memcpy(frame + 16, FX_ANNOUNCE, sizeof FX_ANNOUNCE);
	return len;
}

static size_t flood(const uint8_t mac[6], uint16_t ctr, int width)
{
	return reac_ctrl_build_flood_filler(frame, BCAST, mac, ctr, width, NULL, 0);
}

static const struct reac_disco_entry *entry(const struct reac_hunt *h, const uint8_t mac[6])
{
	for (int i = 0; i < h->table.n; i++)
		if (memcmp(h->table.e[i].mac, mac, 6) == 0)
			return &h->table.e[i];
	return NULL;
}

/* Feed `len` bytes of `frame` at `t` and re-decide. */
static enum reac_hunt_verdict at(struct reac_hunt *h, size_t len, uint64_t t)
{
	struct reac_disco_sighting s;
	CHK(len > 0 && reac_hunt_observe(h, frame, len, t, &s) >= 0);
	reac_hunt_step(h, t);
	return h->verdict;
}

static void flood_only_box(int width)
{
	uint8_t mac[6] = { 0x00, 0x40, 0xab, 0x0f, 0x00, (uint8_t)width };
	const uint64_t W = reac_master_only_cadence_ns(0);
	struct reac_hunt h;
	reac_hunt_init(&h, US, T0);
	uint16_t c = 1;
	for (uint64_t t = T0; t < T0 + W; t += 50 * MS) {
		CHK(at(&h, flood(mac, c++, width), t) == REAC_HUNT_HUNTING);   /* held */
		const struct reac_disco_entry *e = entry(&h, mac);
		CHK(e && e->role == REAC_DISCO_ROLE_UNKNOWN && e->channels == (unsigned)width);
		CHK(reac_sender_kind(e, t, 0) == REAC_RIVAL_UNKNOWN);
		CHK(reac_sender_kind(e, t, REAC_CFG_RATE_48000 / REAC_SAMPLES_PER_PKT) ==
		    REAC_RIVAL_UNKNOWN);
	}
	/* The cadence ran out with no master-only op: a box, at every pace. */
	at(&h, flood(mac, c++, width), T0 + W);
	CHK(reac_sender_kind(entry(&h, mac), T0 + W, 0) == REAC_RIVAL_BOX);
	CHK(reac_sender_kind(entry(&h, mac), T0 + W, REAC_CFG_RATE_96000 / REAC_SAMPLES_PER_PKT)
	    == REAC_RIVAL_BOX);
	/* ...and once the hunt's own vacancy window closes, the wire is driven for it. */
	CHK(at(&h, flood(mac, c++, width), T0 + REAC_HUNT_WINDOW_NS) == REAC_HUNT_MASTER);
}

int main(void)
{
	/* ---- 1. the window is the fact, in frames, at each pace ---- */
	static const struct { int rate; uint32_t frames; } pace[] = {
		{ REAC_CFG_RATE_44100, REAC_MASTER_ONLY_CADENCE_FRAMES_44K1 },
		{ REAC_CFG_RATE_48000, REAC_MASTER_ONLY_CADENCE_FRAMES_48K },
		{ REAC_CFG_RATE_96000, REAC_MASTER_ONLY_CADENCE_FRAMES_96K },
	};
	uint64_t longest = 0;
	for (unsigned i = 0; i < 3; i++) {
		const int fps = pace[i].rate / REAC_SAMPLES_PER_PKT;
		const uint64_t ns = (uint64_t)pace[i].frames * 1000000000ULL / (uint64_t)fps;
		CHK(reac_master_only_cadence_frames(fps) == pace[i].frames);
		CHK(reac_master_only_cadence_ns(fps) == ns);   /* the frames, at this pace */
		if (ns > longest)
			longest = ns;
	}
	/* No pace known: the longest of the three windows, so the hold is never short. */
	CHK(reac_master_only_cadence_frames(0) == 0);
	CHK(reac_master_only_cadence_ns(0) == longest);
	CHK(reac_master_only_cadence_ns(0) < REAC_HUNT_WINDOW_NS);        /* not the hunt's */

	/* ---- 2. a desk whose master op arrives inside the window is the desk ---- */
	{
		static const uint8_t DESK[6] = { 0x00, 0x40, 0xab, 0xc9, 0x91, 0x9c };
		const uint64_t W = reac_master_only_cadence_ns(0);
		struct reac_hunt h;
		reac_hunt_init(&h, US, T0);
		uint16_t c = 1;
		uint64_t t = T0;
		for (; t + 50 * MS < T0 + W; t += 50 * MS) {
			CHK(at(&h, desk_audio(DESK, c++), t) == REAC_HUNT_HUNTING);   /* held */
			CHK(reac_sender_kind(entry(&h, DESK), t, 0) == REAC_RIVAL_UNKNOWN);
		}
		CHK(t < T0 + W);                                  /* premise: inside the window */
		CHK(at(&h, desk_announce(DESK, c++), t) == REAC_HUNT_SLAVE);
		CHK(reac_sender_kind(entry(&h, DESK), t, 0) == REAC_RIVAL_DESK);
		CHK(h.arb.state == REAC_SEGMENT_FOREIGN && h.arb.rival == REAC_RIVAL_DESK);
		t = T0 + 5 * W;                                   /* proof does not expire */
		CHK(at(&h, desk_audio(DESK, c++), t) == REAC_HUNT_SLAVE);
		CHK(reac_sender_kind(entry(&h, DESK), t, 0) == REAC_RIVAL_DESK);
	}

	/* ---- 3. a flood-only box is a box after the window, whatever its width ---- */
	flood_only_box(16);
	flood_only_box(REAC_BOX_MAX_CHANNELS);   /* 40: the desk's width, still a box */

	/* ---- 4. the limit is the limit ---- */
	{
		static const uint8_t LATE[6] = { 0x00, 0x40, 0xab, 0x1a, 0x7e, 0x01 };
		const uint64_t W = reac_master_only_cadence_ns(0);
		struct reac_hunt h;
		reac_hunt_init(&h, US, T0);
		uint16_t c = 1;
		uint64_t t = T0;
		for (; t <= T0 + W; t += 50 * MS)
			at(&h, desk_audio(LATE, c++), t);
		CHK(reac_sender_kind(entry(&h, LATE), t, 0) == REAC_RIVAL_BOX);
		CHK(at(&h, desk_announce(LATE, c++), t) == REAC_HUNT_SLAVE);   /* late proof */
		CHK(reac_sender_kind(entry(&h, LATE), t, 0) == REAC_RIVAL_DESK);
	}

	if (fails) {
		fprintf(stderr, "%d hold check(s) failed\n", fails);
		return 1;
	}
	printf("OK: test_hold — the hold is one master-only cadence, %u / %u / %u frames at "
	       "44.1 / 48 / 96 kHz (a protocol fact); a desk whose master op arrives inside it is "
	       "joined as the desk; a flood-only box is held inside it and is a box past it, at "
	       "16 and at 40 wide; a late master op still makes its sender the desk\n",
	       REAC_MASTER_ONLY_CADENCE_FRAMES_44K1, REAC_MASTER_ONLY_CADENCE_FRAMES_48K,
	       REAC_MASTER_ONLY_CADENCE_FRAMES_96K);
	return 0;
}
