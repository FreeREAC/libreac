// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* A SCENE REPLAY STAYS INSIDE THE HEAD-AMP TABLE. reac_headamp_tx_arm_scene() replays
 * base..base+width-1 clamped to the table. Its clamp used to compute the width as
 * REAC_HEADAMP_MAX_CH - base in a uint8_t, so a base past the table wrapped into a width
 * and the replay walked off the end and round to channel 0: armed at 0xf9, it wrote
 * head-amp records to channels 249..255 and then to every channel 0..47.
 *
 *   base 0x20, width 16   the positive control: channels 32..47, each with all three
 *                         parameters, 48 records, nothing else;
 *   base 40, width 16     clamped at the ceiling: channels 40..47, 24 records;
 *   base 48, 49, 255      at or past the table: no record at all.
 *
 * Every arm drains the scheduler for far longer than any replay takes. */
#include <reac/reac_headamp_tx.h>

#include <stdio.h>
#include <string.h>

static int fails;
#define CHK(c) do { \
	if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
} while (0)

#define DRAIN_FRAMES 100000

/* Arm a replay on a fresh table and count what it emits, per channel. Returns the
 * record count; `per_ch` gets one count per channel 0..255 and `bad` the records whose
 * parameter or value the table could not hold. */
static int replay(uint8_t base, uint8_t width, int per_ch[256], int *bad)
{
	struct reac_headamp_tx t;
	reac_headamp_tx_init(&t);
	reac_headamp_tx_arm_scene(&t, base, width);
	memset(per_ch, 0, 256 * sizeof per_ch[0]);
	*bad = 0;
	int n = 0;
	for (int i = 0; i < DRAIN_FRAMES; i++) {
		uint8_t ch, param, value;
		if (reac_headamp_tx_next(&t, &ch, &param, &value) != 1)
			continue;
		n++;
		per_ch[ch]++;
		if (param >= REAC_HEADAMP_NPARAMS || value != reac_headamp_default(param))
			(*bad)++;
	}
	return n;
}

static int count_in(const int per_ch[256], int lo, int hi)
{
	int n = 0;
	for (int c = lo; c <= hi; c++)
		n += per_ch[c];
	return n;
}

int main(void)
{
	int per_ch[256], bad;

	/* the positive control: an admitted S-1608 grant at base 0x20 */
	int n = replay(0x20, 16, per_ch, &bad);
	CHK(n == 16 * REAC_HEADAMP_NPARAMS);
	CHK(count_in(per_ch, 32, 47) == n);
	for (int c = 32; c <= 47; c++)
		CHK(per_ch[c] == REAC_HEADAMP_NPARAMS);
	CHK(bad == 0);
	if (n == 0) {
		fprintf(stderr, "test_headamp_replay: NOT A RESULT -- the control replay emitted nothing\n");
		return 2;
	}

	/* clamped at the ceiling, not wrapped */
	n = replay(40, 16, per_ch, &bad);
	CHK(n == 8 * REAC_HEADAMP_NPARAMS);
	CHK(count_in(per_ch, 40, 47) == n);
	CHK(bad == 0);

	/* at or past the table: nothing */
	static const uint8_t PAST[] = { 48, 49, 255 };
	for (size_t i = 0; i < sizeof PAST; i++) {
		n = replay(PAST[i], 8, per_ch, &bad);
		CHK(n == 0);
		n = replay(PAST[i], 0xf9, per_ch, &bad);
		CHK(n == 0);
	}

	if (fails) {
		fprintf(stderr, "test_headamp_replay: %d check(s) failed\n", fails);
		return 1;
	}
	printf("test_headamp_replay: PASS -- a replay stays inside the head-amp table\n");
	return 0;
}
