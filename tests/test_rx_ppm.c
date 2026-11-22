// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The published ppm_error_milli is an int, and update_ppm used to cast
 * ppm * 1000.0 to it unchecked. A master running faster than about 3.1x
 * (+2.1e6 ppm) or a NaN/inf estimate (a zero sample rate) made that cast
 * undefined behaviour. The conversion must saturate at the int range and
 * refuse a non-finite estimate to 0 ("no information"). Run it under
 * -fsanitize=undefined to see the cast itself. */
#include "reac_rx_ppm.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>

static int fails;
#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* volatile: the compiler must not fold the cast away, so UBSan sees it run. */
static int conv(double ppm) { volatile double v = ppm; return reac_rx_ppm_to_milli(v); }

int main(void)
{
	CHK(conv(0.0) == 0);
	CHK(conv(18.25) == 18250);
	CHK(conv(-900.0) == -900000);
	CHK(conv(2000000.0) == 2000000000);          /* ~3x: still representable */
	/* A master at 4x (+3e6 ppm), 100x, and the double extremes. */
	CHK(conv(3000000.0) == INT_MAX);
	CHK(conv(-3000000.0) == INT_MIN);
	CHK(conv(9.9e7) == INT_MAX);
	CHK(conv(1e300) == INT_MAX);
	CHK(conv(-1e300) == INT_MIN);
	CHK(conv(INFINITY) == 0);
	CHK(conv(-INFINITY) == 0);
	CHK(conv(NAN) == 0);
	if (fails) return 1;
	printf("OK: ppm_error_milli saturates at the int range, non-finite refused to 0\n");
	return 0;
}
