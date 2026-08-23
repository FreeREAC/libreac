// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Bind libreac's constants to the protocol schema, and hold the head-amp
 * granularities to what is actually known about each of them.
 *
 * reac_facts_assert.h is GENERATED in FreeREAC/reac-protocol from
 * spec/protocol-facts.yaml, where every constant carries its evidence grade. It
 * is copied here rather than adopted wholesale: taking the full reac_facts.h in
 * place of our hand-written #defines is a real change and this migration is not
 * finished. The assertions cost one include and fail BY NAME on any drift, which
 * is the part worth having today.
 *
 * When the schema moves, re-copy this file. It is the schema's, not ours — the
 * header says so itself. */

#include "reac_facts_assert.h"
#include <stdio.h>

#define CHK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL: %s (line %d)\n", #x, __LINE__); return 1; } } while (0)

int main(void)
{
	/* ALL THREE PARAMETERS REACH EVERY CHANNEL. Phantom joined the other two on
	 * 2026-08-23: this loop used to run over SENS and PAD only, while phantom
	 * had a loop of its own asserting that the API REFUSED to answer for three
	 * channels in four. It is one loop now because there is one law.
	 *
	 * The count is the load-bearing part. The retracted reading made a sweep
	 * emit 12 phantom records over a 48-channel space instead of 48, so a test
	 * that only checked "carries" per channel would pass under both readings —
	 * it is the TOTAL that separates them. */
	int carried = 0;
	for (int ch = 0; ch < REAC_HEADAMP_MAX_CH; ch++) {
		CHK(reac_headamp_record_carries((unsigned char)ch, REAC_HEADAMP_SENS) == 1);
		CHK(reac_headamp_record_carries((unsigned char)ch, REAC_HEADAMP_PAD) == 1);
		CHK(reac_headamp_record_carries((unsigned char)ch, REAC_HEADAMP_PHANTOM) == 1);
		CHK(reac_headamp_group_of((unsigned char)ch, REAC_HEADAMP_SENS) == ch);
		CHK(reac_headamp_group_of((unsigned char)ch, REAC_HEADAMP_PAD) == ch);
		CHK(reac_headamp_group_of((unsigned char)ch, REAC_HEADAMP_PHANTOM) == ch);
		carried++;
	}
	CHK(carried == REAC_HEADAMP_MAX_CH);           /* 48 of 48, not 12 */

	/* THE CHANNELS THAT DISCRIMINATE. A per-four reading and a per-channel one
	 * agree on every multiple of four and nowhere else, so the evidence is the
	 * other three: 0x26 is the channel a real M-200i named six times while
	 * toggling one input's phantom, and 0x26 & 3 == 2. */
	CHK(reac_headamp_group_of(0x25, REAC_HEADAMP_PHANTOM) == 0x25);
	CHK(reac_headamp_group_of(0x26, REAC_HEADAMP_PHANTOM) == 0x26);
	CHK(reac_headamp_group_of(0x27, REAC_HEADAMP_PHANTOM) == 0x27);

	/* The per-four field this used to be folded into is the INVENTORY CELL: a
	 * declaration of what a group of four connectors is, and not a head-amp
	 * parameter. It lives in reac_ports.h and spans the same 48 channels. */
	CHK(REAC_PORTS_TABLE_SLOTS * REAC_PORTS_CH_PER_SLOT == REAC_HEADAMP_MAX_CH);

	/* The readback nibble is a third axis, per eight, and stays named apart —
	 * now the one head-amp shift that is not zero. */
	CHK(REAC_HEADAMP_GRAN_READBACK_SHIFT != REAC_HEADAMP_GRAN_PHANTOM_SHIFT);
	CHK(REAC_HEADAMP_GRAN_PHANTOM_SHIFT == REAC_HEADAMP_GRAN_SENS_SHIFT);
	CHK(REAC_HEADAMP_GRAN_PHANTOM_SHIFT == REAC_HEADAMP_GRAN_FLAGS_SHIFT);

	CHK(reac_headamp_record_carries(0, 0x7f) == -1);   /* not a head-amp param */
	CHK(reac_headamp_group_of(0, 0x7f) == -1);

	printf("OK: schema assertions hold; the head-amp wire record is per channel "
	       "for all three parameters INCLUDING PHANTOM (measured 2026-08-23), "
	       "and the per-four field is the inventory cell\n");
	return 0;
}
