// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test: the ENCODE side — reac_braid_encode + reac_downstream_build.
 *
 * These two moved here from reac-pw (reac_tx_build and the static
 * place_braided_audio of reac_ctrl.c) on 2026-07-29. reac-pw's main
 * auto-deploys to the live rig, so the move had exactly one acceptance
 * criterion: the bytes on the wire must not change. Test 1 is that proof, and
 * it is not "the tests pass" — it is a digest of a large deterministic corpus
 * of frames whose expected value was computed by RUNNING THE PRE-MOVE CODE.
 *
 * 1. GOLDEN. 1120 downstream frames covering every combination of
 *    {0,1,2,7,8,16,32,40} source channels x {0,1,6,12,13} samples x seven
 *    counters (0, 1, 0x1234, 0x7fff, 0x8000, 0xfffe, 0xffff — both sides of
 *    the 16-bit wrap) x four input scales (quiet, full scale, 4x CLIPPING,
 *    near-denormal), with every 5th plane NULL to exercise the silence path.
 *    Frame lengths are absorbed into the digest too, so a length change is
 *    caught as well as a content change. The constant below was produced by
 *    reac-pw at c86a2d6 (pre-move reac_tx_build); reac-pw's own suite pins the
 *    same constant from the other side of the wrap, so the two repos cannot
 *    drift apart silently.
 * 2. ROUND TRIP. reac_braid_encode -> reac_upstream_decode is the identity (up
 *    to 24-bit quantization) for every even box width 2..38 — the encoder and
 *    the decoder are inverses, which is the whole point of both living here.
 * 3. COVERAGE. The encode writes every byte of the audio region exactly once
 *    (bijection), so a memset + encode leaves no stale bytes behind.
 * 4. SILENCE + CLAMP. NULL planar, NULL planes and out-of-range channel
 *    indices encode digital silence; out-of-range floats clamp to the 24-bit
 *    signed range instead of wrapping polarity.
 * 5. FRAME SHAPE. reac_downstream_build's envelope: broadcast dst, src,
 *    EtherType, u16-LE counter, zero control block, C2 EA marker, and audio at
 *    offset exactly 50.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

#include <reac/reac.h>
#include <reac/reac_braid.h>
#include <reac/reac_sample.h>
#include <reac/reac_encode.h>
#include <reac/reac_upstream.h>

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* ---- the byte-identity corpus (shared, verbatim, with reac-pw's suite) ---- */

/* Digest of the pre-move encoder's output over the corpus below. Recomputing
 * it needs the old code, so treat a mismatch as "the wire format changed",
 * never as "update the constant". */
#define DOWNSTREAM_GOLDEN 0xe86e36e1d979f244ULL

#define FNV_INIT  0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

static uint32_t rng_state;
static void rng_seed(uint32_t s) { rng_state = s ? s : 0x2b3c4d5eu; }
static uint32_t rng_u32(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state; }
static float rng_f(float scale)
{
	int32_t v = (int32_t)rng_u32();
	return scale * ((float)v / 2147483648.0f);
}

static uint64_t fnv;
static void absorb(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		fnv = (fnv ^ p[i]) * FNV_PRIME;
}
static void absorb_len(size_t len)
{
	uint8_t l[2] = { (uint8_t)(len & 0xff), (uint8_t)((len >> 8) & 0xff) };
	absorb(l, 2);
}

static const float SCALES[4] = { 0.25f, 1.0f, 4.0f, 0.0000001f };
static const int   NS[5]     = { 0, 1, 6, 12, 13 };
static const uint16_t CNT[7] = { 0x0000, 0x0001, 0x1234, 0x7fff, 0x8000, 0xfffe, 0xffff };

static float chbuf[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
static float *planar[REAC_MAX_CHANNELS];

static float *const *fill(uint32_t seed, float scale, int null_all)
{
	rng_seed(seed);
	for (int ch = 0; ch < REAC_MAX_CHANNELS; ch++) {
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
			chbuf[ch][s] = rng_f(scale);
		planar[ch] = (ch % 5 == 4) ? NULL : chbuf[ch];
	}
	return null_all ? NULL : (float *const *)planar;
}

static uint64_t downstream_corpus(void)
{
	static const int NCH[8] = { 0, 1, 2, 7, 8, 16, 32, 40 };
	static uint8_t frame[REAC_FRAME_BYTES];
	uint32_t seed = 1;

	fnv = FNV_INIT;
	for (int si = 0; si < 4; si++)
		for (int ci = 0; ci < 8; ci++)
			for (int ni = 0; ni < 5; ni++)
				for (int ti = 0; ti < 7; ti++) {
					int nch = NCH[ci];
					/* planar==NULL only with nch==0: the pre-move encoder
					 * dereferenced planar[ch] for ch<nch, so a NULL planar with
					 * channels was never a legal call and is not pinned here. */
					float *const *pl = fill(seed++, SCALES[si], nch == 0);
					uint8_t src[6] = { 0x00, 0x40, 0xab,
					                   (uint8_t)si, (uint8_t)ci, (uint8_t)(ni * 8 + ti) };
					int len = reac_downstream_build(frame, pl, nch, NS[ni], CNT[ti], src);
					absorb_len((size_t)len);
					absorb(frame, (size_t)len);
				}
	return fnv;
}

/* ---- helpers ---- */

static int32_t s24_at(const uint8_t *planar_pcm, int ch, int s)
{
	const uint8_t *p = planar_pcm + ((size_t)ch * REAC_SAMPLES_PER_PKT + s) * 3;
	int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
	if (v & 0x00800000)
		v |= ~0x00FFFFFF;
	return v;
}

int main(void)
{
	/* ---- 1. the golden: bytes are what the pre-move encoder emitted ---- */
	{
		uint64_t got = downstream_corpus();
		if (got != DOWNSTREAM_GOLDEN) {
			fails++;
			fprintf(stderr, "FAIL downstream corpus digest: got 0x%016llx want 0x%016llx\n"
			                "  the emitted BYTES changed — this is a wire-format regression\n",
			        (unsigned long long)got, (unsigned long long)DOWNSTREAM_GOLDEN);
		}
	}

	/* ---- 2. encode -> decode is the identity, every box width ---- */
	for (int nch = 2; nch < REAC_MAX_CHANNELS; nch += 2) {
		size_t len = REAC_UPSTREAM_OVERHEAD + (size_t)nch * REAC_UPSTREAM_BYTES_PER_CH;
		uint8_t frame[REAC_FRAME_BYTES];
		uint8_t out[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT * 3];
		float in[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
		float *pl[REAC_MAX_CHANNELS];

		rng_seed((uint32_t)nch + 7u);
		for (int ch = 0; ch < nch; ch++) {
			for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
				in[ch][s] = rng_f(0.9f);
			pl[ch] = in[ch];
		}

		/* a minimal legal upstream envelope around the encoded region */
		memset(frame, 0, len);
		frame[12] = 0x88; frame[13] = 0x19;
		frame[len - 2] = REAC_END_MARKER_0;
		frame[len - 1] = REAC_END_MARKER_1;
		reac_braid_encode(frame + REAC_AUDIO_OFFSET, nch, pl, nch, REAC_SAMPLES_PER_PKT);

		CHK(reac_upstream_channels(len) == nch);
		CHK(reac_upstream_decode(frame, len, out) == REAC_SAMPLES_PER_PKT);
		for (int ch = 0; ch < nch; ch++)
			for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
				/* one 24-bit ULP is the only permitted difference */
				int32_t want = (int32_t)lrintf(in[ch][s] * 8388608.0f);
				CHK(s24_at(out, ch, s) == want);
			}
	}

	/* ---- 3. the encode covers the region exactly ---- */
	for (int nch = 2; nch <= REAC_MAX_CHANNELS; nch += 2) {
		size_t region = (size_t)nch * REAC_UPSTREAM_BYTES_PER_CH;
		uint8_t a[REAC_AUDIO_BYTES], b[REAC_AUDIO_BYTES];
		float in[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
		float *pl[REAC_MAX_CHANNELS];

		rng_seed(0xC0FFEEu + (uint32_t)nch);
		for (int ch = 0; ch < nch; ch++) {
			for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
				in[ch][s] = rng_f(0.5f);
			pl[ch] = in[ch];
		}
		/* Two different pre-fills must give the same result: every byte of the
		 * region is written, so nothing of the old content survives. */
		memset(a, 0x00, region);
		memset(b, 0xA5, region);
		reac_braid_encode(a, nch, pl, nch, REAC_SAMPLES_PER_PKT);
		reac_braid_encode(b, nch, pl, nch, REAC_SAMPLES_PER_PKT);
		CHK(memcmp(a, b, region) == 0);
	}

	/* ---- 4. silence + clamp ---- */
	{
		uint8_t a[REAC_AUDIO_BYTES], z[REAC_AUDIO_BYTES];
		float hot[REAC_SAMPLES_PER_PKT], cold[REAC_SAMPLES_PER_PKT];
		float *pl[4];

		memset(z, 0, sizeof z);

		/* NULL planar -> silence for the whole region */
		memset(a, 0x5A, sizeof a);
		reac_braid_encode(a, REAC_MAX_CHANNELS, NULL, 0, REAC_SAMPLES_PER_PKT);
		CHK(memcmp(a, z, REAC_AUDIO_BYTES) == 0);

		/* n_src smaller than n_ch -> the surplus channels are silent */
		memset(a, 0x5A, sizeof a);
		reac_braid_encode(a, REAC_MAX_CHANNELS, pl, 0, REAC_SAMPLES_PER_PKT);
		CHK(memcmp(a, z, REAC_AUDIO_BYTES) == 0);

		/* ns <= 0 writes nothing at all (the caller owns the buffer state) */
		memset(a, 0x5A, sizeof a);
		reac_braid_encode(a, REAC_MAX_CHANNELS, NULL, 0, 0);
		for (size_t i = 0; i < REAC_AUDIO_BYTES; i++)
			CHK(a[i] == 0x5A);

		/* clamp, not wrap: +4.0 must read back as full-scale POSITIVE */
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) { hot[s] = 4.0f; cold[s] = -4.0f; }
		pl[0] = hot; pl[1] = cold; pl[2] = hot; pl[3] = cold;
		memset(a, 0, sizeof a);
		reac_braid_encode(a, 4, pl, 4, REAC_SAMPLES_PER_PKT);
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
			size_t pos[3];
			uint8_t t[3];
			reac_braid_pos(s, 0, 4, pos);
			t[0] = a[pos[0]]; t[1] = a[pos[1]]; t[2] = a[pos[2]];
			CHK(reac_s24le_to_f32(t) > 0.999f);
			reac_braid_pos(s, 1, 4, pos);
			t[0] = a[pos[0]]; t[1] = a[pos[1]]; t[2] = a[pos[2]];
			CHK(reac_s24le_to_f32(t) <= -1.0f);
		}
	}

	/* ---- 5. the downstream envelope ---- */
	{
		static const uint8_t src[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0xf6 };
		uint8_t f[REAC_FRAME_BYTES];
		float sig[REAC_SAMPLES_PER_PKT];
		float *pl[1] = { sig };

		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
			sig[s] = 0.5f;

		CHK(reac_downstream_build(f, pl, 1, REAC_SAMPLES_PER_PKT, 0xBEEF, src)
		    == REAC_FRAME_BYTES);
		for (int i = 0; i < 6; i++)
			CHK(f[i] == 0xFF);                      /* broadcast dst */
		CHK(memcmp(f + 6, src, 6) == 0);            /* our src */
		CHK(f[12] == 0x88 && f[13] == 0x19);        /* EtherType */
		CHK(f[REAC_HDR_COUNTER_OFF] == 0xEF);       /* counter, LITTLE endian */
		CHK(f[REAC_HDR_COUNTER_OFF + 1] == 0xBE);
		CHK(f[16] == 0x00 && f[17] == 0x00);        /* type 0x0000 = FILLER */
		for (int i = 18; i < REAC_AUDIO_OFFSET; i++)
			CHK(f[i] == 0x00);                      /* control block left to the caller */
		CHK(f[REAC_FRAME_BYTES - 2] == REAC_END_MARKER_0);
		CHK(f[REAC_FRAME_BYTES - 1] == REAC_END_MARKER_1);

		/* audio at offset exactly 50, ch 0 only, everything else silent */
		CHK(reac_frame_is_reac(f, REAC_FRAME_BYTES));
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
			size_t pos[3];
			uint8_t t[3];
			reac_braid_pos(s, 0, REAC_MAX_CHANNELS, pos);
			t[0] = f[REAC_AUDIO_OFFSET + pos[0]];
			t[1] = f[REAC_AUDIO_OFFSET + pos[1]];
			t[2] = f[REAC_AUDIO_OFFSET + pos[2]];
			CHK(reac_s24le_to_f32(t) > 0.4999f && reac_s24le_to_f32(t) < 0.5001f);
			for (int ch = 1; ch < REAC_MAX_CHANNELS; ch++) {
				reac_braid_pos(s, ch, REAC_MAX_CHANNELS, pos);
				CHK(f[REAC_AUDIO_OFFSET + pos[0]] == 0);
				CHK(f[REAC_AUDIO_OFFSET + pos[1]] == 0);
				CHK(f[REAC_AUDIO_OFFSET + pos[2]] == 0);
			}
		}
	}

	if (fails) {
		fprintf(stderr, "test_encode: %d failure(s)\n", fails);
		return 1;
	}
	printf("test_encode: OK\n");
	return 0;
}
