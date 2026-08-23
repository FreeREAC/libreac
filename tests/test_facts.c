// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Bind libreac's constants to the protocol schema, and check the three head-amp
 * granularities behave.
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
	/* SENS and the flags reach every channel. */
	for (int ch = 0; ch < REAC_HEADAMP_MAX_CH; ch++) {
		CHK(reac_headamp_record_carries((unsigned char)ch, REAC_HEADAMP_SENS) == 1);
		CHK(reac_headamp_record_carries((unsigned char)ch, REAC_HEADAMP_PAD) == 1);
		CHK(reac_headamp_group_of((unsigned char)ch, REAC_HEADAMP_SENS) == ch);
	}

	/* Phantom reaches only the first channel of each group of four: a record to
	 * 0x24 moves group 9, one to 0x25/0x26/0x27 moves nothing. */
	int carried = 0;
	for (int ch = 0; ch < REAC_HEADAMP_MAX_CH; ch++) {
		int c = reac_headamp_record_carries((unsigned char)ch, REAC_HEADAMP_PHANTOM);
		CHK(c == ((ch % 4 == 0) ? 1 : 0));
		carried += c;
		CHK(reac_headamp_group_of((unsigned char)ch, REAC_HEADAMP_PHANTOM) == ch / 4);
	}
	CHK(carried == REAC_HEADAMP_MAX_CH / 4);          /* 12 of 48 */

	/* The S-1608's own bank, which is where this was measured: sixteen phantom
	 * records to 0x20..0x2f, four of which do anything. */
	int live = 0;
	for (int ch = 0x20; ch <= 0x2f; ch++)
		live += reac_headamp_record_carries((unsigned char)ch, REAC_HEADAMP_PHANTOM);
	CHK(live == 4);

	CHK(reac_headamp_record_carries(0, 0x7f) == -1);   /* not a head-amp param */

	printf("OK: schema assertions hold; head-amp granularity is per channel for "
	       "SENS/flags and per four for phantom (4 of 16 live on 0x20..0x2f)\n");
	return 0;
}
