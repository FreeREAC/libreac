// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The capacity of the caller's output buffer is a parameter the callee checks.
 * reac_downstream_build writes REAC_FRAME_BYTES (1492) and reac_decode writes
 * n_channels * samples_per_pkt * 3 (1440) bytes; before, the capacity lived in a
 * comment and a short buffer was an overwrite. A refusal returns -1 and writes
 * nothing (a canary around the buffer proves it); an exact-fit buffer works and
 * its far side is untouched. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <reac/reac.h>
#include <reac/reac_encode.h>
#include <reac/reac_decode.h>

static int fails;
#define CHK(c) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define GUARD 64
static const uint8_t SRC[6] = { 0x00, 0x40, 0xab, 0x01, 0x02, 0x03 };

static int pristine(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i] != 0xA5)
			return 0;
	return 1;
}

int main(void)
{
	/* ---- reac_downstream_build ---- */
	uint8_t buf[REAC_FRAME_BYTES + GUARD];
	memset(buf, 0xA5, sizeof buf);
	CHK(reac_downstream_build(buf, REAC_FRAME_BYTES - 1, NULL, 0, 0, 1, SRC) == -1);
	CHK(pristine(buf, sizeof buf));
	CHK(reac_downstream_build(buf, 0, NULL, 0, 0, 1, SRC) == -1);
	CHK(reac_downstream_build(NULL, REAC_FRAME_BYTES, NULL, 0, 0, 1, SRC) == -1);
	CHK(reac_downstream_build(buf, REAC_FRAME_BYTES, NULL, 0, 0, 1, NULL) == -1);
	CHK(pristine(buf, sizeof buf));
	CHK(reac_downstream_build(buf, REAC_FRAME_BYTES, NULL, 0, 0, 1, SRC) == REAC_FRAME_BYTES);
	CHK(buf[REAC_FRAME_BYTES - 2] == REAC_END_MARKER_0 && buf[REAC_FRAME_BYTES - 1] == REAC_END_MARKER_1);
	CHK(pristine(buf + REAC_FRAME_BYTES, GUARD));

	/* ---- reac_decode, on the frame just built ---- */
	const size_t need = (size_t)REAC_MODE_48K.n_channels * REAC_MODE_48K.samples_per_pkt * REAC_RESOLUTION;
	uint8_t pcm[1440 + GUARD];
	CHK(need == 1440);
	memset(pcm, 0xA5, sizeof pcm);
	CHK(reac_decode(buf, REAC_FRAME_BYTES, &REAC_MODE_48K, pcm, need - 1) == -1);
	CHK(reac_decode(buf, REAC_FRAME_BYTES, &REAC_MODE_48K, pcm, 0) == -1);
	CHK(reac_decode(buf, REAC_FRAME_BYTES, &REAC_MODE_48K, NULL, need) == -1);
	CHK(pristine(pcm, sizeof pcm));
	CHK(reac_decode(buf, REAC_FRAME_BYTES, &REAC_MODE_48K, pcm, need) == REAC_MODE_48K.samples_per_pkt);
	CHK(pristine(pcm + need, GUARD));
	return fails ? 1 : 0;
}
