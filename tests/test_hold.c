// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* HOLD WITH A DECLARED LIMIT. Operator ruling 2026-09-25, on the one case width used to
 * settle and can no longer (a box may be 40 wide): a BROADCAST audio stream whose source
 * has sent nothing that names its role is the desk's downstream or a box's
 * presence-flood. Hold the verdict until the sender's own frames prove its role, but
 * never longer than k x the desk's declared master-announce interval; at the window's
 * end a broadcast sender with no master-only frame is a BOX.
 *
 * The interval is reac-protocol's ANNOUNCE_PERIOD_MS (timing group), read through the
 * generated reac_facts_timing.h; k is REAC_DESK_ANNOUNCES_TO_WAIT. Driven through the
 * real doors — reac_hunt_observe / reac_hunt_step and reac_sender_kind — on the hunt's
 * own clock:
 *
 *   1. the window is the fact times k, and the hunt's window is the same number;
 *   2. A DESK WHOSE ANNOUNCE ARRIVES INSIDE THE WINDOW IS THE DESK: before its announce
 *      the hunt holds (neither drives nor joins), on it the hunt joins it;
 *   3. A FLOOD-ONLY BOX IS A BOX AFTER THE WINDOW, at 16 wide and at 40 wide: held
 *      (UNKNOWN, HUNTING) inside it, BOX and driven (MASTER) past it;
 *   4. the limit is the limit: a sender whose announce comes only AFTER the window was
 *      a box meanwhile, and its first master-only frame still makes it the desk. */
#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_encode.h>
#include <reac/reac_hunt.h>
#include <reac/reac_arbitration.h>
#include <reac/reac_facts_timing.h>

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

/* A flood-only box of `width`, every 100 ms for `ms` milliseconds. */
static void flood_only_box(int width)
{
	uint8_t mac[6] = { 0x00, 0x40, 0xab, 0x0f, 0x00, (uint8_t)width };
	struct reac_hunt h;
	reac_hunt_init(&h, US, T0);
	const uint64_t W = REAC_DESK_PROOF_WINDOW_NS;
	uint16_t c = 1;
	for (uint64_t t = T0; t < T0 + W; t += 100 * MS) {
		CHK(at(&h, flood(mac, c++, width), t) == REAC_HUNT_HUNTING);   /* held */
		const struct reac_disco_entry *e = entry(&h, mac);
		CHK(e && e->role == REAC_DISCO_ROLE_UNKNOWN && e->channels == (unsigned)width);
		CHK(reac_sender_kind(e, t) == REAC_RIVAL_UNKNOWN);
	}
	/* The window ran out and no master-only frame came: a box, and the wire is driven. */
	const uint64_t past = T0 + W;
	CHK(at(&h, flood(mac, c++, width), past) == REAC_HUNT_MASTER);
	CHK(reac_sender_kind(entry(&h, mac), past) == REAC_RIVAL_BOX);
}

int main(void)
{
	/* ---- 1. the window is declared, and it is one number ---- */
	CHK(REAC_DESK_PROOF_WINDOW_NS ==
	    (uint64_t)REAC_DESK_ANNOUNCES_TO_WAIT * REAC_ANNOUNCE_PERIOD_MS * MS);
	CHK(REAC_HUNT_WINDOW_NS == REAC_DESK_PROOF_WINDOW_NS);
	CHK(REAC_DESK_PROOF_WINDOW_NS > REAC_ANNOUNCE_PERIOD_MS * MS);   /* > one announce */

	/* ---- 2. a desk whose announce arrives inside the window is the desk ---- */
	{
		static const uint8_t DESK[6] = { 0x00, 0x40, 0xab, 0xc9, 0x91, 0x9c };
		struct reac_hunt h;
		reac_hunt_init(&h, US, T0);
		uint16_t c = 1;
		uint64_t t = T0;
		/* its downstream audio first: 40 wide, broadcast — and still unproven */
		for (; t < T0 + REAC_ANNOUNCE_PERIOD_MS * MS; t += 100 * MS) {
			CHK(at(&h, desk_audio(DESK, c++), t) == REAC_HUNT_HUNTING);
			CHK(reac_sender_kind(entry(&h, DESK), t) == REAC_RIVAL_UNKNOWN);
		}
		CHK(t < T0 + REAC_DESK_PROOF_WINDOW_NS);    /* premise: still inside the window */
		/* its announce: a master-only frame, inside the window */
		CHK(at(&h, desk_announce(DESK, c++), t) == REAC_HUNT_SLAVE);
		CHK(reac_sender_kind(entry(&h, DESK), t) == REAC_RIVAL_DESK);
		CHK(h.arb.state == REAC_SEGMENT_FOREIGN && h.arb.rival == REAC_RIVAL_DESK);
		/* and it stays the desk past the window: proof does not expire */
		t = T0 + 2 * REAC_DESK_PROOF_WINDOW_NS;
		CHK(at(&h, desk_audio(DESK, c++), t) == REAC_HUNT_SLAVE);
		CHK(reac_sender_kind(entry(&h, DESK), t) == REAC_RIVAL_DESK);
	}

	/* ---- 3. a flood-only box is a box after the window, whatever its width ---- */
	flood_only_box(16);
	flood_only_box(REAC_BOX_MAX_CHANNELS);   /* 40: the desk's width, still a box */

	/* ---- 4. the limit is the limit ---- */
	{
		static const uint8_t LATE[6] = { 0x00, 0x40, 0xab, 0x1a, 0x7e, 0x01 };
		struct reac_hunt h;
		reac_hunt_init(&h, US, T0);
		uint16_t c = 1;
		uint64_t t = T0;
		for (; t <= T0 + REAC_DESK_PROOF_WINDOW_NS; t += 100 * MS)
			at(&h, desk_audio(LATE, c++), t);
		CHK(reac_sender_kind(entry(&h, LATE), t) == REAC_RIVAL_BOX);
		CHK(h.verdict == REAC_HUNT_MASTER);   /* the declared cost of the limit */
		/* its first master-only frame, late, still makes it the desk */
		CHK(at(&h, desk_announce(LATE, c++), t) == REAC_HUNT_SLAVE);
		CHK(reac_sender_kind(entry(&h, LATE), t) == REAC_RIVAL_DESK);
	}

	if (fails) {
		fprintf(stderr, "%d hold check(s) failed\n", fails);
		return 1;
	}
	printf("OK: test_hold — the window is %d x REAC_ANNOUNCE_PERIOD_MS (%d ms, a protocol "
	       "fact) and the hunt's is the same; a desk that announces inside it is joined as "
	       "the desk; a flood-only box is held inside it and driven as a box past it, at 16 "
	       "and at 40 wide; a late announce still makes its sender the desk\n",
	       REAC_DESK_ANNOUNCES_TO_WAIT, REAC_ANNOUNCE_PERIOD_MS);
	return 0;
}
