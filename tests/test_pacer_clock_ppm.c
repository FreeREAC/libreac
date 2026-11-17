// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_pacer_clock_publish_graph turns a driver's rate_diff into the published
 * ppm_milli int. It cast (ppm * 1000.0) unchecked: a NaN rate_diff (NaN <= 0 is
 * false, so reac_clock_ppm_from_rate_diff passes it through) or a huge one
 * (a driver at 4x, +3e6 ppm) is undefined behaviour. The value must be the same
 * saturation update_ppm applies: INT_MAX/INT_MIN at the range, 0 for non-finite
 * ("no information"). Build with -fsanitize=undefined,float-cast-overflow to see
 * the cast itself; without it the wrong value (INT_MIN) is the evidence. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <reac/reac.h>
#include <reac/transport/reac_pacer.h>

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int published(struct reac_pacer *p, double rate_diff)
{
	volatile double v = rate_diff;   /* the compiler must not fold the cast away */
	reac_pacer_clock_publish_graph(p, "alsa_input.pci-0000_00_1f.3", 0, v, 1000, NULL);
	return atomic_load(&p->clock_ppm_milli[REAC_CLOCK_SRC_GRAPH]);
}

/* (rate - 1) * 1e6 * 1000 is a double product: allow the last milli of rounding. */
static int near(int got, int want) { return got >= want - 1 && got <= want + 1; }

int main(void)
{
	struct reac_pacer *p = calloc(1, sizeof *p);
	if (!p) return 2;
	p->clock_follow = 1;

	CHK(published(p, 1.0) == 0);                     /* CONTROL: on-rate */
	CHK(near(published(p, 1.000018), 18000));        /* +18 ppm */
	CHK(near(published(p, 0.9999), -100000));        /* -100 ppm */
	CHK(published(p, NAN) == 0);
	CHK(published(p, INFINITY) == 0);              /* non-finite: no information */
	CHK(published(p, 4.0) == INT_MAX);               /* +3e6 ppm -> 3e9 milli */
	CHK(published(p, 1e300) == INT_MAX);
	CHK(published(p, 0.0) == 0);                     /* not measured yet */

	free(p);
	if (fails) return 1;
	printf("OK: the graph clock's ppm_milli saturates and refuses non-finite\n");
	return 0;
}
