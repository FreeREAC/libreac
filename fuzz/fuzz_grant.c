// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libFuzzer harness: THE GRANT (src/reac_grant.c) — where a box's announced head-amp
 * base and width become slots, and slots become the enrolment sweep — with the
 * head-amp table that supplies the sweep's values (src/reac_headamp_tx.c).
 *
 * The base and the width are wire facts: the box announces its strap and declares
 * its inputs, and the master admits or refuses them here. The harness hands the
 * grant any base and width, a head-amp table set from the input, and a sweep buffer
 * of an input-chosen size, in its own allocation of exactly that many rows.
 *
 * Input:  [0..1] s16 LE base  [2] in_ch  [3] s8 rows in the sweep buffer
 *         [4] how many head-amp cells to set, then that many [ch][param][value]
 *         [..] a raw allocation [base][width] handed to the sweep builder as a
 *         caller would build one by hand, a scene replay [base][width], a u32 LE
 *         re-assert period, then the number of scheduler frames to run (u16 LE, at
 *         most 4096)
 *
 * Properties, each an abort():
 *   - a box is admitted exactly when its slots [base, base+width) are a positive
 *     width of at most REAC_GRANT_MAX_WIDTH inside the head-amp space (0..0x2f):
 *     REFUSED, never moved;
 *   - a sweep is REAC_GRANT_SWEEP_LEN(width) rows, refused when the buffer is
 *     smaller, and every row is a cd ea link-4 SINGLE whose block sums to zero;
 *   - its group-A rows are, in the measured order, one head-amp record per
 *     allocated channel per parameter, each parsed back by reac_ctrl_parse as
 *     HEADAMP for that channel, parameter and the table's effective value, with
 *     its inner record checksum closed;
 *   - the head-amp scheduler only ever emits a channel, parameter and value the
 *     table can hold.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <reac/reac_ctrlblk.h>
#include <reac/reac_grant.h>
#include <reac/reac_headamp_tx.h>
#include <reac/reac_slots.h>

#include "fuzz.h"

#define MAX_FRAMES 4096

static int value_ok(uint8_t param, uint8_t value)
{
	return param == REAC_HEADAMP_SENS ? value <= REAC_HEADAMP_SENS_MAX
	                                  : param < REAC_HEADAMP_NPARAMS && value <= 1;
}

static void check_row(const uint8_t row[34])
{
	unsigned s = 0;
	for (int i = 2; i < 34; i++)
		s += row[i];
	if (row[0] != 0xcd || row[1] != 0xea || row[2] != REAC_LINK_RECORD ||
	    row[3] != REAC_SEG_SINGLE || (s & 0xff) != 0)
		abort();
}

/* One group-A row, framed as a master stamps it and read back by the parser. */
static void check_groupa(const uint8_t row[34], uint8_t ch, uint8_t param,
                         const struct reac_headamp_tx *tx)
{
	uint8_t frame[50] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x40, 0xab, 0, 0, 1,
	                      0x88, 0x19 };
	memcpy(frame + 16, row, 34);
	struct reac_ctrl_parsed p;
	if (reac_ctrl_parse(frame, sizeof frame, &p) != REAC_CTRL_HEADAMP || p.ch != ch ||
	    p.param != param || p.value != reac_headamp_tx_effective(tx, ch, param) ||
	    reac_ctrl_headamp_record_verify(frame) != 0 || reac_ctrl_checksum_verify(frame) != 0)
		abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size >= FUZZ_INPUT_MAX)
		return 0;
	struct fuzz_in in = fuzz_in(data, size);
	int base = (int16_t)fuzz_u16(&in);
	int in_ch = fuzz_u8(&in);
	int rows = (int8_t)fuzz_u8(&in);

	struct reac_headamp_tx *tx = malloc(sizeof *tx);
	if (!tx)
		abort();
	reac_headamp_tx_init(tx);
	int nset = fuzz_u8(&in);
	for (int i = 0; i < nset; i++) {
		uint8_t ch = fuzz_u8(&in), param = fuzz_u8(&in), value = fuzz_u8(&in);
		int r = reac_headamp_tx_set(tx, ch, param, value);
		/* An unknown parameter has no value and no default. */
		if (param >= REAC_HEADAMP_NPARAMS &&
		    (reac_headamp_tx_effective(tx, ch, param) != 0 || reac_headamp_default(param) != 0))
			abort();
		if (r != ((ch < REAC_HEADAMP_MAX_CH && value_ok(param, value)) ? 0 : -1))
			abort();
		if (r == 0 && reac_headamp_tx_effective(tx, ch, param) != value)
			abort();
	}

	const int fits = in_ch > 0 && in_ch <= REAC_GRANT_MAX_WIDTH && base >= 0 &&
	                 base + in_ch - 1 <= REAC_HEADAMP_CEILING;
	if (reac_grant_alloc_fits(base, in_ch) != fits)
		abort();
	struct reac_grant_alloc a = { 0, 0 };
	int r = reac_grant_allocate(&a, base, in_ch);
	if (r != (fits ? 0 : -1))
		abort();
	if (r == 0 && (a.base != base || a.width != in_ch ||
	               a.base + a.width - 1 > REAC_HEADAMP_CEILING))
		abort();   /* admitted where it asked, and inside the head-amp space */

	if (r == 0) {
		uint8_t (*sweep)[34] = malloc((size_t)(rows > 0 ? rows : 1) * 34);
		if (!sweep)
			abort();
		int n = reac_grant_build_sweep(sweep, rows, &a, tx);
		int want = REAC_GRANT_SWEEP_LEN(a.width);
		if (n != (rows >= want ? want : -1))
			abort();
		if (n > 0) {
			for (int k = 0; k < n; k++)
				check_row(sweep[k]);
			/* HEAD_ACK | A[base].0..2 | HEAD_MARK | B x6 | A[base+1..].0..2 */
			for (uint8_t p = 0; p < REAC_HEADAMP_NPARAMS; p++)
				check_groupa(sweep[1 + p], a.base, p, tx);
			for (int c = 1; c < a.width; c++)
				for (uint8_t p = 0; p < REAC_HEADAMP_NPARAMS; p++)
					check_groupa(sweep[11 + (c - 1) * 3 + p],
					             (uint8_t)(a.base + c), p, tx);
			for (int i = 0; i < REAC_GRANT_GROUPB_LEN; i++) {
				uint8_t poll[34];
				if (reac_ctrl_identity_poll_block(i, poll) != 0 ||
				    memcmp(poll, sweep[5 + i], 34) != 0)
					abort();   /* group B is the identity poll, byte for byte */
			}
		}
		free(sweep);
	}

	/* The builder judges an allocation it did not make: a hand-built one is admitted
	 * on the same terms as reac_grant_allocate's. */
	{
		struct reac_grant_alloc raw;
		raw.base = fuzz_u8(&in);
		raw.width = fuzz_u8(&in);
		uint8_t (*sweep)[34] = malloc((size_t)REAC_GRANT_SWEEP_MAX * 34);
		if (!sweep)
			abort();
		int n = reac_grant_build_sweep(sweep, REAC_GRANT_SWEEP_MAX, &raw, tx);
		if (n != (reac_grant_alloc_fits(raw.base, raw.width) ? REAC_GRANT_SWEEP_LEN(raw.width)
		                                                      : -1))
			abort();
		if (reac_grant_build_sweep(NULL, 1, &raw, tx) != -1 ||
		    reac_grant_build_sweep(sweep, REAC_GRANT_SWEEP_MAX, NULL, tx) != -1)
			abort();
		free(sweep);
	}

	uint8_t sb = fuzz_u8(&in), sw = fuzz_u8(&in);
	reac_headamp_tx_arm_scene(tx, sb, sw);
	reac_headamp_tx_set_resweep(tx, fuzz_u32(&in));
	int frames = fuzz_u16(&in) % (MAX_FRAMES + 1);
	for (int i = 0; i < frames; i++) {
		uint8_t ch = 0xff, param = 0xff, value = 0xff;
		int e = reac_headamp_tx_next(tx, &ch, &param, &value);
		if (e != 0 && e != 1)
			abort();
		if (e == 1 && (ch >= REAC_HEADAMP_MAX_CH || !value_ok(param, value)))
			abort();
	}
	free(tx);
	return 0;
}
