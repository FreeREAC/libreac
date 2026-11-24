// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_frame_ring_init rounds `slots` up to a power of two. For a request above
 * 2^31 that ceiling is 2^32, which wraps to 0: a 0-slot calloc (non-NULL under
 * glibc) with mask 0xFFFFFFFF, and the first push writes far outside it.
 * reac_ring_init guards exactly this; the frame ring did not. The door must
 * refuse a request the ring cannot hold and leave nothing allocated. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <reac/reac.h>
#include <reac/transport/reac_pacer.h>

static int fails;
#define CHK(c) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void refused(uint32_t slots, uint32_t slot_sz, const char *what)
{
	struct reac_frame_ring r;
	int rc = reac_frame_ring_init(&r, slots, slot_sz);
	printf("%-28s slots=%u slot_sz=%u -> rc=%d slots=%u mask=0x%x\n",
	       what, slots, slot_sz, rc, r.slots, r.mask);
	CHK(rc == -1);
	CHK(r.buf == NULL && r.len == NULL);
	if (rc == 0) {
		/* the base's accepted ring: show what a push does to it */
		uint8_t f[REAC_FRAME_BYTES] = {0};
		reac_frame_ring_push(&r, f, REAC_FRAME_BYTES);
		reac_frame_ring_free(&r);
	}
}

int main(void)
{
	refused(0x80000001u, REAC_PACER_SLOT_SZ, "rounding overflow");
	refused(0xFFFFFFFFu, REAC_PACER_SLOT_SZ, "max request");
	refused(0, REAC_PACER_SLOT_SZ, "zero slots");
	refused(64, 0, "zero slot size");
	refused(64, REAC_FRAME_BYTES - 1, "slot below one frame");

	struct reac_frame_ring r;
	CHK(reac_frame_ring_init(&r, 64, REAC_PACER_SLOT_SZ) == 0);
	CHK(r.slots == 64 && r.mask == 63);
	uint8_t f[REAC_FRAME_BYTES]; memset(f, 0x5a, sizeof f);
	CHK(reac_frame_ring_push(&r, f, REAC_FRAME_BYTES) == 1);
	uint8_t out[REAC_PACER_SLOT_SZ];
	CHK(reac_frame_ring_pop(&r, out) == REAC_FRAME_BYTES && out[0] == 0x5a);
	reac_frame_ring_free(&r);
	return fails ? 1 : 0;
}
