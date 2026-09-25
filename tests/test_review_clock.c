// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* REVIEW 2026-09-25, finding M2 (docs/audits/2026-09-25-libreac-review.md).
 *
 * reac_clock_disc_update() resets in_band and the stability series when the
 * selected reference CHANGES, but leaves `state` alone unless a measurement
 * arrives in the same call. reac_pacer calls it with have_measurement = 0
 * whenever the newly selected source has no fresh stamp yet
 * (transport/src/reac_pacer.c: `have` is set only on a new clock_stamp_ns), so
 * a discipline LOCKED to the box that gains a PHC reports "locked to NIC/external
 * PHC" — and reac_pace_from_clock() publishes pace `phc` — before a single PHC
 * sample has been taken. The lock belongs to the old reference. */
#include <reac/reac_clock.h>
#include <reac/reac_arbitration.h>

#include <stdio.h>

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

int main(void)
{
	struct reac_clock_disc c;
	reac_clock_disc_init(&c, REAC_ROLE_MASTER, 250000);

	/* Lock on the box's counter slope: in-band samples until LOCKED. */
	for (int i = 0; i < 2 * REAC_DLL_LOCK_UPDATES; i++)
		reac_clock_disc_update(&c, REAC_CLOCK_AVAIL_BOX, 0.0, 1);
	if (c.state != REAC_CLOCK_LOCKED || c.src != REAC_CLOCK_SRC_BOX) {
		printf("NOT A RESULT: test_review_clock — the control never locked to the box\n");
		return 2;
	}

	/* The PHC appears. It outranks the box in the master hierarchy, so it is
	 * selected at once, but it has produced no sample yet. */
	reac_clock_disc_update(&c, REAC_CLOCK_AVAIL_BOX | REAC_CLOCK_AVAIL_PHC, 0.0, 0);
	CHK(c.src == REAC_CLOCK_SRC_PHC);
	CHK(c.in_band == 0);
	CHK(c.state != REAC_CLOCK_LOCKED);   /* no PHC sample: cannot be locked to it */
	CHK(reac_pace_from_clock(c.src, c.state) != REAC_PACE_PHC);

	char buf[256];
	reac_clock_disc_describe(&c, buf, sizeof buf);
	if (fails) {
		fprintf(stderr, "  the discipline now says: \"%s\" after 0 PHC samples\n", buf);
		return 1;
	}
	printf("OK: test_review_clock — a newly selected reference is not reported locked before it is measured\n");
	return 0;
}
