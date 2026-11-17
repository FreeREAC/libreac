// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ppm_seq counts PUBLISHED estimates: a consumer that steers off ppm_error_milli
 * keys "is there an estimate yet" and freshness off it (reac_rx.h). A window whose
 * estimate is not finite (a zero sample rate divides to inf, or 0/0 to NaN) is no
 * estimate, so update_ppm must neither store a value nor bump the counter: a bumped
 * counter vouching for a "0" that was never measured reads as "the reference agrees
 * with us". reac_rx_ppm_to_milli already refuses to 0; the counter was still bumped.
 *
 * update_ppm is static, so this test compiles reac_rx.c into itself. A finite
 * estimate is the CONTROL: it must bump, or the harness proves nothing. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../transport/src/reac_rx.c"

#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define WINDOW_NS 300000000ull   /* past the 250 ms recompute threshold */

int main(void)
{
	struct reac_rx *rx = calloc(1, sizeof *rx);
	if (!rx) return 2;

	/* CONTROL: a sane 48 kHz stream, one frame per update, closes one window. */
	rx->sample_rate = 48000;
	update_ppm(rx, 0, 0);
	update_ppm(rx, 1, WINDOW_NS);
	CHK(atomic_load(&rx->ppm_seq) == 1);          /* a finite estimate is published */

	/* A zero sample rate: nominal pps 0, observed > 0 -> +inf. Not an estimate. */
	rx->sample_rate = 0;
	atomic_store(&rx->ppm_error_milli, 4242);
	uint32_t before = atomic_load(&rx->ppm_seq);
	update_ppm(rx, 2, 2 * WINDOW_NS);
	CHK(atomic_load(&rx->ppm_seq) == before);
	CHK(atomic_load(&rx->ppm_error_milli) == 4242);   /* the old estimate is left alone */

	/* 0/0 -> NaN: nominal pps 0 and no frames in the window. The counter wraps the
	 * 32-bit frame count to exactly 0 (UINT32_MAX + one frame). */
	rx->ppm_win_frames = UINT32_MAX;
	rx->ppm_last_counter = 2;
	before = atomic_load(&rx->ppm_seq);
	update_ppm(rx, 2, 3 * WINDOW_NS);
	CHK(atomic_load(&rx->ppm_seq) == before);
	CHK(atomic_load(&rx->ppm_error_milli) == 4242);

	/* The window still restarts, so the next finite window publishes again. */
	rx->sample_rate = 48000;
	update_ppm(rx, 3, 4 * WINDOW_NS);
	CHK(atomic_load(&rx->ppm_seq) == before + 1);

	free(rx);
	if (fails) return 1;
	printf("OK: a non-finite estimate does not bump ppm_seq\n");
	return 0;
}
