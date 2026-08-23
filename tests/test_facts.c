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
	/* SENS and the flags reach every channel, and they are the settled half. */
	for (int ch = 0; ch < REAC_HEADAMP_MAX_CH; ch++) {
		CHK(reac_headamp_record_carries((unsigned char)ch, REAC_HEADAMP_SENS) == 1);
		CHK(reac_headamp_record_carries((unsigned char)ch, REAC_HEADAMP_PAD) == 1);
		CHK(reac_headamp_group_of((unsigned char)ch, REAC_HEADAMP_SENS) == ch);
		CHK(reac_headamp_group_of((unsigned char)ch, REAC_HEADAMP_PAD) == ch);
	}

	/* PHANTOM'S ACTUATION GRANULARITY IS OPEN and the API says so rather than
	 * answering. The two readings — per four from an executed trace, per channel
	 * from the box's own per-slot table — agree on a group-of-four anchor and
	 * disagree on the other three in four, so that is where it answers and where
	 * it refuses. NEVER 0: "the write lands nowhere" is a claim nobody is
	 * entitled to make while this stands, and it is the claim that made a sweep
	 * skip three records in four. */
	int answered = 0, disputed = 0;
	for (int ch = 0; ch < REAC_HEADAMP_MAX_CH; ch++) {
		int c = reac_headamp_record_carries((unsigned char)ch, REAC_HEADAMP_PHANTOM);
		CHK(c == 1 || c == REAC_HEADAMP_GRAN_DISPUTED);
		CHK(c == ((ch % 4 == 0) ? 1 : REAC_HEADAMP_GRAN_DISPUTED));
		answered += (c == 1);
		disputed += (c == REAC_HEADAMP_GRAN_DISPUTED);
		CHK(reac_headamp_group_of((unsigned char)ch, REAC_HEADAMP_PHANTOM)
		    == REAC_HEADAMP_GRAN_DISPUTED);
	}
	CHK(answered == REAC_HEADAMP_MAX_CH / 4);      /* 12 of 48 */
	CHK(disputed == REAC_HEADAMP_MAX_CH - answered);

	/* The per-four field this used to be folded into is the INVENTORY CELL: a
	 * declaration of what a group of four connectors is, and not a head-amp
	 * parameter. It lives in reac_ports.h and spans the same 48 channels. */
	CHK(REAC_PORTS_TABLE_SLOTS * REAC_PORTS_CH_PER_SLOT == REAC_HEADAMP_MAX_CH);

	/* The readback nibble is a third axis, per eight, and stays named apart. */
	CHK(REAC_HEADAMP_GRAN_READBACK_SHIFT != REAC_HEADAMP_GRAN_PHANTOM_SHIFT_TRACE);

	CHK(reac_headamp_record_carries(0, 0x7f) == -1);   /* not a head-amp param */
	CHK(reac_headamp_group_of(0, 0x7f) == -1);

	printf("OK: schema assertions hold; the head-amp wire record is per channel "
	       "for all three parameters, the per-four field is the inventory cell, "
	       "and phantom's actuation granularity comes back DISPUTED off an "
	       "anchor rather than as a number\n");
	return 0;
}
