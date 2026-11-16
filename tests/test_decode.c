// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test: the DOWNSTREAM decode — reac_decode (braid, the default since
 * 0.5.0).
 *
 * The defect this pins (libreac#13): reac_downstream_build() wrote the braid,
 * reac_decode() read plain LE, and on a 1492 B frame the library had just built
 * itself 0 of 480 samples agreed. So the acceptance criterion is a ROUND TRIP
 * through the library's own encoder, not a hand-written byte fixture.
 *
 * 1. ROUND TRIP, exact. reac_downstream_build -> reac_decode returns all 480
 *    samples bit-exact. The targets are integer multiples of 1024 divided by
 *    2^23, so they survive float and lrintf without rounding: a mismatch can
 *    only be layout. Every one of the 480 targets is DISTINCT, so an exact
 *    match at all 480 positions also rules out any permutation of them — the
 *    failure a wrong stride produces. Run at all three mode descriptors.
 * 2. NO CROSS-WIRE, sharpened. One channel hot, the other 39 silent, for each
 *    of the 40 channels in turn: the hot value must land on that channel and
 *    every other channel must decode to exact digital silence. A stride or
 *    pair-parity error delivers the value to a neighbour and this catches it
 *    even when the values are not all distinct.
 * 3. VALIDATION. Malformed frames reject, and the braid geometry guard rejects
 *    a descriptor the braid has no byte map for (odd width) or that does not
 *    fit the 1440 B audio region.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include <reac/reac.h>
#include <reac/reac_braid.h>
#include <reac/reac_sample.h>
#include <reac/reac_encode.h>
#include <reac/reac_decode.h>

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define NCH REAC_MAX_CHANNELS
#define NS  REAC_SAMPLES_PER_PKT

/* 2^23: the scale reac_sample.h converts on. A target of k*1024/2^23 with
 * integer k is exactly representable as a float AND lands on an exact s24
 * code, so nothing in the round trip can round. */
#define S24_SCALE 8388608.0f

static const uint8_t SRC[6] = { 0x00, 0x40, 0xab, 0x01, 0x02, 0x03 };

static uint8_t frame[REAC_FRAME_BYTES];
static uint8_t pcm[NCH * NS * 3];
static float chbuf[NCH][NS];
static float *planar[NCH];

/* the s24 code stored for channel ch, time-sample s in a planar decode result */
static int32_t s24_at(const uint8_t *p, int ch, int s)
{
	const uint8_t *q = p + ((size_t)ch * NS + s) * 3;
	int32_t v = (int32_t)((uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16));
	if (v & 0x00800000)
		v |= ~0x00FFFFFF;
	return v;
}

/* target code for (ch,s): distinct across all 480 positions, exactly on a
 * 24-bit code, and far enough from zero that silence can never look like it */
static int32_t target_code(int ch, int s) { return (int32_t)((ch * NS + s + 1) * 1024); }

int main(void)
{
	/* ---- 1. round trip: what the builder wrote, the decoder reads back ---- */
	{
		static const struct reac_mode *const MODES[3] = {
			&REAC_MODE_44K1, &REAC_MODE_48K, &REAC_MODE_96K
		};
		for (int ch = 0; ch < NCH; ch++) {
			for (int s = 0; s < NS; s++)
				chbuf[ch][s] = (float)target_code(ch, s) / S24_SCALE;
			planar[ch] = chbuf[ch];
		}
		CHK(reac_downstream_build(frame, (float *const *)planar, NCH, NS,
		                          0x1234, SRC) == REAC_FRAME_BYTES);

		for (int m = 0; m < 3; m++) {
			memset(pcm, 0xA5, sizeof pcm);
			CHK(reac_decode(frame, REAC_FRAME_BYTES, MODES[m], pcm) == NS);
			int exact = 0;
			for (int ch = 0; ch < NCH; ch++)
				for (int s = 0; s < NS; s++)
					if (s24_at(pcm, ch, s) == target_code(ch, s))
						exact++;
			if (exact != NCH * NS) {
				fails++;
				fprintf(stderr, "FAIL round trip @%d Hz: %d/%d samples exact\n",
				        MODES[m]->sample_rate, exact, NCH * NS);
			}
		}
	}

	/* ---- 2. no cross-wire: one hot channel at a time ---- */
	for (int hot = 0; hot < NCH; hot++) {
		static float sig[NS];
		const int32_t code = (int32_t)((hot + 1) * 8192);

		for (int s = 0; s < NS; s++)
			sig[s] = (float)code / S24_SCALE;
		for (int ch = 0; ch < NCH; ch++)
			planar[ch] = (ch == hot) ? sig : NULL;

		reac_downstream_build(frame, (float *const *)planar, NCH, NS, 0, SRC);
		CHK(reac_decode(frame, REAC_FRAME_BYTES, &REAC_MODE_48K, pcm) == NS);
		for (int ch = 0; ch < NCH; ch++)
			for (int s = 0; s < NS; s++)
				CHK(s24_at(pcm, ch, s) == (ch == hot ? code : 0));
	}

	/* ---- 3. validation + the braid geometry guard ---- */
	{
		static uint8_t bad[REAC_FRAME_BYTES];
		struct reac_mode odd = { 48000, 39, NS };     /* the braid packs PAIRS */
		struct reac_mode wide = { 48000, 42, NS };    /* 42*12*3 > 1440 B */
		struct reac_mode deep = { 48000, NCH, 13 };   /* 40*13*3 > 1440 B */
		struct reac_mode empty = { 48000, 0, NS };

		reac_downstream_build(bad, NULL, 0, NS, 0, SRC);
		CHK(reac_decode(bad, REAC_FRAME_BYTES, &REAC_MODE_48K, pcm) == NS);

		CHK(reac_decode(bad, REAC_FRAME_BYTES - 1, &REAC_MODE_48K, pcm) == -1);
		CHK(reac_decode(bad, REAC_FRAME_BYTES_OHRCA, &REAC_MODE_48K, pcm) == -1);
		bad[12] = 0x08;                               /* wrong ethertype */
		CHK(reac_decode(bad, REAC_FRAME_BYTES, &REAC_MODE_48K, pcm) == -1);
		bad[12] = 0x88;
		bad[REAC_FRAME_BYTES - 1] = 0x00;             /* broken end marker */
		CHK(reac_decode(bad, REAC_FRAME_BYTES, &REAC_MODE_48K, pcm) == -1);
		bad[REAC_FRAME_BYTES - 1] = REAC_END_MARKER_1;

		CHK(reac_decode(bad, REAC_FRAME_BYTES, &odd, pcm) == -1);
		CHK(reac_decode(bad, REAC_FRAME_BYTES, &wide, pcm) == -1);
		CHK(reac_decode(bad, REAC_FRAME_BYTES, &deep, pcm) == -1);
		CHK(reac_decode(bad, REAC_FRAME_BYTES, &empty, pcm) == -1);

		/* reac_frame_inspect is unchanged: it reads the counter of a good frame */
		reac_downstream_build(bad, NULL, 0, NS, 0xBEEF, SRC);
		struct reac_frame f = reac_frame_inspect(bad, REAC_FRAME_BYTES, &REAC_MODE_48K);
		CHK(f.valid == 1 && f.counter == 0xBEEF);
	}

	if (fails) {
		fprintf(stderr, "test_decode: %d failure(s)\n", fails);
		return 1;
	}
	printf("test_decode: OK — downstream round trip exact at 44k1/48k/96k, "
	       "no cross-wire over 40 hot-channel cases\n");
	return 0;
}
