// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test: reac_braid_encode keeps inside the audio region it was given.
 *
 * The braid packs channel PAIRS and a frame is at most 40 channels wide, so a
 * width that is odd, below 2 or above 40 has no legal region. The encoder must
 * refuse such a width (and a buffer too small for the width) and touch nothing,
 * not write past the end of the caller's buffer.
 *
 * `make test SANITIZE=1` builds this file WITH -fsanitize=address,undefined and compiles
 * src/reac_encode.c itself with the same flags (Makefile), so a write past the
 * end of a heap buffer sized exactly for the region is an ASan report, not a
 * silent pass. Every buffer here is malloc'd to the exact size the case claims.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <reac/reac.h>
#include <reac/reac_encode.h>

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define REGION(n) ((size_t)(n) * REAC_SAMPLES_PER_PKT * 3)

/* One encode into an exact-size heap buffer holding `len` bytes. The encoder
 * must refuse (-1) and leave every byte as it found it. */
static void encode_into(size_t len, int n_ch)
{
	uint8_t *buf = malloc(len ? len : 1);
	float plane[REAC_SAMPLES_PER_PKT] = { 0 };
	float *pl[REAC_MAX_CHANNELS];

	if (!buf)
		abort();   /* not a property: an allocation failed */
	memset(buf, 0x5A, len);
	for (int i = 0; i < REAC_MAX_CHANNELS; i++)
		pl[i] = plane;
	CHK(reac_braid_encode(buf, len, n_ch, pl, REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT) == -1);
	for (size_t i = 0; i < len; i++)
		CHK(buf[i] == 0x5A);
	free(buf);
}

/* Optional argv[1] runs one case (0..6) so each defect shows its own ASan report. */
int main(int argc, char **argv)
{
	int only = argc > 1 ? atoi(argv[1]) : -1;
	static const struct { size_t len; int n_ch; const char *what; } cases[] = {
		/* odd widths: the region is n_ch * 36 bytes but the braid's last even
		 * channel writes one byte beyond it (n_ch = 1: byte 36 of 36). */
		{ REGION(1),  1,  "odd width 1" },
		{ REGION(3),  3,  "odd width 3" },
		{ REGION(39), 39, "odd width 39" },
		/* above the frame width: the audio region of a full frame is 1440 bytes */
		{ REGION(REAC_MAX_CHANNELS), 42, "width 42 into a 1440-byte region" },
		{ REGION(REAC_MAX_CHANNELS), 64, "width 64 into a 1440-byte region" },
		/* a legal width into a buffer too small for it */
		{ REGION(8) - 1, 8, "width 8, buffer one byte short" },
		{ REGION(REAC_MAX_CHANNELS) - 1, REAC_MAX_CHANNELS, "width 40, buffer one byte short" },
	};

	for (int i = 0; i < (int)(sizeof cases / sizeof *cases); i++) {
		if (only >= 0 && only != i)
			continue;
		fprintf(stderr, "case %d: %s\n", i, cases[i].what);
		encode_into(cases[i].len, cases[i].n_ch);
	}

	/* and the legal edges still encode: width 2 and 40, into exactly their region */
	if (only < 0) {
		for (int n = 2; n <= REAC_MAX_CHANNELS; n += 38) {
			uint8_t *buf = malloc(REGION(n));
			float plane[REAC_SAMPLES_PER_PKT] = { 0 };
			float *pl[REAC_MAX_CHANNELS];

			if (!buf)
				abort();
			for (int i = 0; i < REAC_MAX_CHANNELS; i++)
				pl[i] = plane;
			CHK(reac_braid_encode(buf, REGION(n), n, pl, REAC_MAX_CHANNELS,
			                      REAC_SAMPLES_PER_PKT) == 0);
			free(buf);
		}
		/* a NULL region is refused, not written */
		CHK(reac_braid_encode(NULL, REGION(8), 8, NULL, 0, REAC_SAMPLES_PER_PKT) == -1);
	}

	if (fails)
		return 1;
	puts("test_braid_width: PASS");
	return 0;
}
