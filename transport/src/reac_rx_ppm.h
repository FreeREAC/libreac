// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_rx_ppm — the one conversion from a ppm estimate (a double) to a published
 * ppm * 1000 (an int): update_ppm's ppm_error_milli and the pacer's graph-clock
 * ppm_milli both go through it. Pure, header-only, no allocation, no logging:
 * both run on RT or feeder threads and a test drives this same function. */
#ifndef REAC_RX_PPM_H
#define REAC_RX_PPM_H

#include <limits.h>
#include <math.h>

/* A non-finite estimate (a zero sample rate divides to NaN/inf) is "no information":
 * 0. A finite one saturates at the int range, so a master running faster than about
 * 3.1x cannot make the cast undefined behaviour. */
static inline int reac_rx_ppm_to_milli(double ppm)
{
	if (!isfinite(ppm))
		return 0;
	double milli = ppm * 1000.0;
	if (milli >= (double)INT_MAX)
		return INT_MAX;
	if (milli <= (double)INT_MIN)
		return INT_MIN;
	return (int)milli;
}

#endif /* REAC_RX_PPM_H */
