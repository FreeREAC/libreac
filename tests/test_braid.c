// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test: the braid layout oracle + the sample conversion pair + the OHRCA
 * +2 trailer strip.
 *
 * 1. reac_braid_pos is BIJECTIVE over the whole n_ch*36-byte audio region for
 *    every even width 2..40 (each byte written exactly once) — the property the
 *    encode side relies on (silence + placed channels covers the region).
 * 2. The braid byte map matches the reference definition verbatim: even channel
 *    (lo,mid,hi) at group[3],group[0],group[1], odd at group[4],group[5],group[2],
 *    group at (s*n_ch + (ch&~1))*3 (obs-h8819 convert_to_pcm24lep / reacdriver
 *    16-bit-word swap — see reac_braid.h for the evidence trail).
 * 3. reac_f32_to_s24le / reac_s24le_to_f32 are exact inverses over the full
 *    24-bit range (every s24 value round-trips), and the encode clamps.
 * 4. reac_frame_clean_len strips exactly the OHRCA +2 (1494->1492, 1206->1204,
 *    630->628) and leaves clean/invalid lengths untouched.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <reac/reac.h>
#include <reac/reac_braid.h>
#include <reac/reac_sample.h>

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

int main(void)
{
	/* 1. bijectivity per width: every byte of the region hit exactly once */
	for (int nch = 2; nch <= REAC_MAX_CHANNELS; nch += 2) {
		uint8_t hit[REAC_MAX_CHANNELS * REAC_UPSTREAM_BYTES_PER_CH];
		size_t region = (size_t)nch * REAC_UPSTREAM_BYTES_PER_CH;
		memset(hit, 0, sizeof hit);
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
			for (int ch = 0; ch < nch; ch++) {
				size_t pos[3];
				reac_braid_pos(s, ch, nch, pos);
				for (int i = 0; i < 3; i++) {
					if (pos[i] >= region) { fails++; continue; }
					hit[pos[i]]++;
				}
			}
		int once = 1;
		for (size_t i = 0; i < region; i++)
			if (hit[i] != 1)
				once = 0;
		CHK(once);
	}

	/* 2. the byte map, spelled out against the reference definition */
	size_t pos[3];
	reac_braid_pos(0, 0, 16, pos);                    /* s0 ch0: group at 0 */
	CHK(pos[0] == 3 && pos[1] == 0 && pos[2] == 1);
	reac_braid_pos(0, 1, 16, pos);                    /* s0 ch1: same group */
	CHK(pos[0] == 4 && pos[1] == 5 && pos[2] == 2);
	reac_braid_pos(0, 2, 16, pos);                    /* next pair group */
	CHK(pos[0] == 2 * 3 + 3 && pos[1] == 2 * 3 + 0 && pos[2] == 2 * 3 + 1);
	reac_braid_pos(1, 0, 16, pos);                    /* next sample: stride nch*3 */
	CHK(pos[0] == 16 * 3 + 3 && pos[1] == 16 * 3 + 0 && pos[2] == 16 * 3 + 1);
	reac_braid_pos(2, 7, 40, pos);                    /* downstream width spot check */
	CHK(pos[0] == (size_t)(2 * 40 + 6) * 3 + 4);

	/* 3. sample pair: every 24-bit value round-trips exactly */
	int roundtrip_ok = 1;
	for (int32_t v = -8388608; v <= 8388607; v += 1) {
		uint8_t b[3] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
		                 (uint8_t)((v >> 16) & 0xFF) };
		uint8_t b2[3];
		reac_f32_to_s24le(reac_s24le_to_f32(b), b2);
		if (memcmp(b, b2, 3) != 0)
			roundtrip_ok = 0;
	}
	CHK(roundtrip_ok);
	/* clamp: out-of-range input pins to the 24-bit rails */
	uint8_t b[3];
	reac_f32_to_s24le(2.0f, b);
	CHK(b[0] == 0xFF && b[1] == 0xFF && b[2] == 0x7F);   /*  8388607 */
	reac_f32_to_s24le(-2.0f, b);
	CHK(b[0] == 0x00 && b[1] == 0x00 && b[2] == 0x80);   /* -8388608 */

	/* 4. OHRCA +2 strip */
	CHK(reac_frame_clean_len(REAC_FRAME_BYTES_OHRCA) == (size_t)REAC_FRAME_BYTES);
	CHK(reac_frame_clean_len(1206) == 1204);  /* S-4000 32-ch trailered */
	CHK(reac_frame_clean_len(630) == 628);    /* S-1608 16-ch trailered */
	CHK(reac_frame_clean_len(1492) == 1492);  /* clean downstream unchanged */
	CHK(reac_frame_clean_len(628) == 628);    /* clean upstream unchanged */
	CHK(reac_frame_clean_len(340) == 340);
	CHK(reac_frame_clean_len(629) == 629);    /* +1 is not the trailer rule */
	CHK(reac_frame_clean_len(0) == 0);        /* short lengths untouched */
	CHK(reac_frame_clean_len(53) == 53);

	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("OK: braid_pos bijective for widths 2..40 + reference byte map, "
	       "f32<->s24 exact round-trip + clamp, OHRCA +2 strip\n");
	return 0;
}
