// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ONE FRAME DOES NOT DECIDE A SEGMENT'S ROLE.
 *
 * A cfea announce at a box width, sent once from any MAC, used to turn a pinned master
 * into a slave (and an unpinned wire into one) on the next hunt step: the rival was a
 * "box on M" from its first frame. A real master, desk or box on M, fills every audio slot
 * at wire cadence, so the hunt now takes a rival into account only once it has streamed
 * (REAC_HUNT_RIVAL_FRAMES over REAC_HUNT_RIVAL_SPAN_NS). Through reac_hunt_observe /
 * reac_hunt_step:
 *
 *   1. a pinned master hearing one forged announce is not a slave; it waits while that
 *      sender is heard, and drives once it has gone quiet (a forged frame cannot hold the
 *      pin off the wire either);
 *   2. the same on an unpinned wire: one announce is not a master on the wire;
 *   3. THE CONTROL: a box on M that streams at cadence is joined by the pinned master,
 *      exactly as the 2026-09-16 ruling says, within ~20 ms of its first frame.
 *
 * Sabotage: make rival_evidence() answer RIVAL_STREAMS always and arms 1 and 2 go red. */
#include <stdio.h>
#include <string.h>

#include <reac/reac.h>
#include <reac/reac_ctrl.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_hunt.h>

static int fails;
#define CHK(c) do { \
	if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
} while (0)

#define US_ 1000ULL
#define MS  1000000ULL
static const uint8_t US[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static uint8_t f[REAC_FRAME_BYTES];

/* A broadcast cfea announce at 8 slots from `mac`, in a box-width (8-channel) frame. */
static size_t cfea(const uint8_t mac[6], uint16_t ctr)
{
	size_t n = 52 + 8 * 36;
	memset(f, 0, sizeof f);
	memset(f, 0xff, 6);
	memcpy(f + 6, mac, 6);
	f[12] = 0x88; f[13] = 0x19;
	f[14] = (uint8_t)ctr; f[15] = (uint8_t)(ctr >> 8);
	f[16] = 0xcf; f[17] = 0xea;
	static const uint8_t b[] = { 0xff, 0xff, 0x01, 0x00, 0x01, 0x03, 0x0d, 0x01, 0x04 };
	memcpy(f + 18, b, sizeof b);
	memcpy(f + 27, mac, 6);
	f[33] = 8; f[34] = 8;
	f[n - 2] = REAC_END_MARKER_0;
	f[n - 1] = REAC_END_MARKER_1;
	reac_ctrl_checksum_apply(f);
	return n;
}

static enum reac_hunt_verdict feed(struct reac_hunt *h, const uint8_t mac[6], uint16_t ctr,
                                   uint64_t t)
{
	size_t n = cfea(mac, ctr);
	CHK(reac_hunt_observe(h, f, n, t, NULL) >= 0);
	reac_hunt_step(h, t);
	return h->verdict;
}

int main(void)
{
	static const uint8_t EVIL[6] = { 0x02, 0x06, 0x06, 0x06, 0x06, 0x06 };
	static const uint8_t BOXM[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x41 };
	const uint64_t t0 = 1000 * MS;

	/* 1. pinned master, one forged announce */
	{
		struct reac_hunt h;
		reac_hunt_init(&h, US, t0);
		reac_hunt_pin(&h, REAC_ROLE_MASTER);
		CHK(feed(&h, EVIL, 1, t0) != REAC_HUNT_SLAVE);
		/* the sender is still being heard: nothing moves yet */
		reac_hunt_step(&h, t0 + 10 * MS);
		CHK(h.verdict != REAC_HUNT_SLAVE);
		/* and gone quiet: the pin drives */
		reac_hunt_step(&h, t0 + REAC_HUNT_RIVAL_QUIET_NS + MS);
		CHK(h.verdict == REAC_HUNT_MASTER);
		CHK(reac_hunt_role(&h) == REAC_ROLE_MASTER);
	}

	/* 2. unpinned wire, one forged announce */
	{
		struct reac_hunt h;
		reac_hunt_init(&h, US, t0);
		CHK(feed(&h, EVIL, 1, t0) != REAC_HUNT_SLAVE);
		reac_hunt_step(&h, t0 + REAC_HUNT_RIVAL_QUIET_NS + MS);
		CHK(h.verdict != REAC_HUNT_SLAVE);
	}

	/* 3. THE CONTROL: a box on M streaming at cadence (one frame per 250 us) is joined by
	 * the pinned master inside ~20 ms */
	{
		struct reac_hunt h;
		reac_hunt_init(&h, US, t0);
		reac_hunt_pin(&h, REAC_ROLE_MASTER);
		enum reac_hunt_verdict v = REAC_HUNT_HUNTING;
		uint64_t joined_at = 0;
		for (int i = 0; i < 400 && v != REAC_HUNT_SLAVE; i++) {
			uint64_t t = t0 + (uint64_t)i * 250 * US_;
			v = feed(&h, BOXM, (uint16_t)i, t);
			if (v == REAC_HUNT_SLAVE)
				joined_at = t - t0;
			else
				CHK(v != REAC_HUNT_MASTER);   /* never drives over a box that streams */
		}
		CHK(v == REAC_HUNT_SLAVE);
		CHK(joined_at <= REAC_HUNT_RIVAL_SPAN_NS + MS);
		printf("  box on M streaming at 4 kHz joined after %.2f ms\n", (double)joined_at / 1e6);
	}

	if (fails) {
		fprintf(stderr, "%d hunt one-frame check(s) failed\n", fails);
		return 1;
	}
	printf("OK: test_hunt_one_frame — one forged cfea makes neither a pinned master nor an "
	       "unpinned wire a slave, and a forged sender gone quiet leaves the pin driving; a box "
	       "on M that streams is joined within %llu ms\n",
	       (unsigned long long)(REAC_HUNT_RIVAL_SPAN_NS / MS));
	return 0;
}
