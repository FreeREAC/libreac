// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac/reac.h"
#include <stdio.h>
#include <stdint.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void)
{
	/* rate snap: pps = rate/12, midpoints at 3837.5 and 6000 */
	CHECK(reac_rate_snap(3675) == 44100, "snap 3675 -> 44100");
	CHECK(reac_rate_snap(4000) == 48000, "snap 4000 -> 48000");
	CHECK(reac_rate_snap(8000) == 96000, "snap 8000 -> 96000");
	CHECK(reac_rate_snap(3837) == 44100, "snap 3837 -> 44100 (below mid)");
	CHECK(reac_rate_snap(3838) == 48000, "snap 3838 -> 48000 (above mid)");
	CHECK(reac_rate_snap(5999) == 48000, "snap 5999 -> 48000");
	CHECK(reac_rate_snap(6001) == 96000, "snap 6001 -> 96000");
	CHECK(reac_rate_snap(7900) == 96000, "snap ~8000 with jitter -> 96000");

	/* mode_for */
	CHECK(reac_mode_for(48000) == &REAC_MODE_48K, "mode_for 48000");
	CHECK(reac_mode_for(96000) == &REAC_MODE_96K, "mode_for 96000");
	CHECK(reac_mode_for(44100) == &REAC_MODE_44K1, "mode_for 44100");
	CHECK(reac_mode_for(12345) == &REAC_MODE_48K, "mode_for unknown -> 48k");
	CHECK(REAC_MODE_96K.n_channels == 40 && REAC_MODE_96K.samples_per_pkt == 12,
	      "96k is 40ch/12samp (settled double-pps, not channel-halving)");

	/* frame helpers */
	uint8_t f[20] = {0};
	f[12] = 0x88; f[13] = 0x19;            /* EtherType 0x8819 */
	f[14] = 0x34; f[15] = 0x12;            /* counter 0x1234, little-endian */
	CHECK(reac_frame_is_reac(f, sizeof f) == 1, "EtherType 0x8819 -> REAC");
	f[13] = 0x00;
	CHECK(reac_frame_is_reac(f, sizeof f) == 0, "wrong EtherType -> not REAC");
	CHECK(reac_frame_is_reac(f, 10) == 0, "too short -> not REAC");
	f[13] = 0x19;
	CHECK(reac_frame_counter(f) == 0x1234, "counter LE 0x1234");
	CHECK(reac_counter_gap(0x1233, 0x1234) == 0, "gap consecutive = 0");
	CHECK(reac_counter_gap(0x1233, 0x1236) == 2, "gap of 2");
	CHECK(reac_counter_gap(0xFFFF, 0x0001) == 1, "gap across 16-bit wrap = 1");

	if (fails == 0) printf("OK: all libreac tests passed\n");
	else printf("%d libreac test(s) failed\n", fails);
	return fails ? 1 : 0;
}
