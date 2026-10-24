// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libFuzzer harness: the AUDIO FRAME decoders (src/reac_decode.c,
 * src/reac_upstream.c, and the frame helpers in src/reac.c).
 *
 * Input:  [0] mode selector  [1] n_channels  [2] samples_per_pkt  [3..] the frame
 *
 * The selector's low two bits pick REAC_MODE_44K1 / _48K / _96K, or (3) a
 * descriptor built from bytes 1 and 2 read as signed: the geometry guard in
 * reac_decode() exists for a caller's descriptor, so the harness has to be able to
 * hand it a bad one. The frame is everything after the header, at its own length,
 * in its own allocation.
 *
 * Properties, each an abort():
 *   - the decoders agree with reac_frame_inspect(): a frame decodes exactly when
 *     inspect calls it valid and the geometry fits, and then returns its ns;
 *   - the braid is a bijection: decoding then re-encoding through
 *     reac_braid_encode() gives back the audio region byte for byte, downstream
 *     and upstream;
 *   - reac_upstream_channels() is reac_frame_channels() restricted to box widths;
 *   - reac_frame_clean_len() only ever strips the two-byte FCS residue.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <reac/reac.h>
#include <reac/reac_braid.h>
#include <reac/reac_decode.h>
#include <reac/reac_encode.h>
#include <reac/reac_sample.h>
#include <reac/reac_upstream.h>

#include "fuzz.h"

/* Decode planar 24-bit PCM back to floats and braid it into a fresh region; the
 * region must come back as the frame carried it. */
static void check_round_trip(const uint8_t *audio, const uint8_t *pcm, int nch, int ns)
{
	size_t region = (size_t)nch * (size_t)ns * REAC_RESOLUTION;
	float *planes = malloc((size_t)nch * (size_t)ns * sizeof *planes);
	float **planar = malloc((size_t)nch * sizeof *planar);
	uint8_t *again = calloc(region ? region : 1, 1);
	if (!planes || !planar || !again)
		abort();
	for (int ch = 0; ch < nch; ch++) {
		planar[ch] = planes + (size_t)ch * (size_t)ns;
		for (int s = 0; s < ns; s++)
			planar[ch][s] = reac_s24le_to_f32(pcm + ((size_t)ch * (size_t)ns + (size_t)s) * 3);
	}
	reac_braid_encode(again, nch, planar, nch, ns);
	/* The braid is a bijection over the n_ch x ns x 3 region and the 24-bit codec
	 * is exact, so decode followed by encode is the identity. */
	if (memcmp(again, audio, region) != 0)
		abort();
	free(again);
	free(planar);
	free(planes);
}

static void fuzz_downstream(const uint8_t *frame, size_t len, const struct reac_mode *mode)
{
	const int nch = mode->n_channels, ns = mode->samples_per_pkt;
	const int fits = nch > 0 && ns > 0 &&
	                 (size_t)nch * (size_t)ns * REAC_RESOLUTION <= (size_t)REAC_AUDIO_BYTES;
	size_t out_len = fits ? (size_t)nch * (size_t)ns * REAC_RESOLUTION : 0;
	uint8_t *out = malloc(out_len ? out_len : 1);
	if (!out)
		abort();

	struct reac_frame f = reac_frame_inspect(frame, len, mode);
	/* inspect's contract: valid only on a 1492 B 0x8819 frame ending C2 EA, and the
	 * counter is bytes 14-15 little-endian. */
	if (f.valid) {
		if (len != REAC_FRAME_BYTES || frame[12] != 0x88 || frame[13] != 0x19 ||
		    frame[len - 2] != REAC_END_MARKER_0 || frame[len - 1] != REAC_END_MARKER_1)
			abort();
		if (f.counter != reac_frame_counter(frame))
			abort();
	}

	int r = reac_decode(frame, len, mode, out);
	/* The braid needs an even width and a geometry inside the region. */
	if (r != ((f.valid && fits && (nch & 1) == 0) ? ns : -1))
		abort();
	if (r > 0 && ns <= REAC_SAMPLES_PER_PKT)
		check_round_trip(frame + REAC_L2_HEADER_LEN, out, nch, ns);

	r = reac_decode_plain_le(frame, len, mode, out);
	/* The plain-LE diagnostic pairs nothing, so only the size bound applies. */
	if (r != ((f.valid && fits) ? ns : -1))
		abort();
	if (r > 0) {
		/* Its meaning, as the header defines it: (ch, s) is the straight copy at
		 * (s * n_channels + ch) * 3. Spot-check the last sample, the one a short
		 * bound would miss. */
		size_t last = ((size_t)(ns - 1) * (size_t)nch + (size_t)(nch - 1)) * REAC_RESOLUTION;
		if (memcmp(out + out_len - 3, frame + REAC_L2_HEADER_LEN + last, 3) != 0)
			abort();
	}
	free(out);
}

static void fuzz_upstream(const uint8_t *frame, size_t len)
{
	int nch = reac_upstream_channels(len);
	unsigned geom = reac_frame_channels(len);
	/* A box's return is a legal geometry at a box width, and nothing else. */
	if (nch != (reac_box_width_ok((int)geom) ? (int)geom : -1))
		abort();

	size_t out_len = nch > 0 ? (size_t)nch * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION : 0;
	uint8_t *out = malloc(out_len ? out_len : 1);
	if (!out)
		abort();
	if (reac_upstream_decode(NULL, len, out) != -1 || reac_upstream_decode(frame, len, NULL) != -1)
		abort();
	int r = reac_upstream_decode(frame, len, out);
	int ok = nch > 0 && frame[12] == 0x88 && frame[13] == 0x19 &&
	         frame[len - 2] == REAC_END_MARKER_0 && frame[len - 1] == REAC_END_MARKER_1;
	if (r != (ok ? REAC_SAMPLES_PER_PKT : -1))
		abort();
	if (r > 0)
		check_round_trip(frame + REAC_L2_HEADER_LEN, out, nch, REAC_SAMPLES_PER_PKT);
	free(out);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size >= FUZZ_INPUT_MAX)
		return 0;
	struct fuzz_in in = fuzz_in(data, size);
	uint8_t sel = fuzz_u8(&in);
	int8_t nch = (int8_t)fuzz_u8(&in);
	int8_t ns = (int8_t)fuzz_u8(&in);

	struct reac_mode custom = { 48000, nch, ns };
	const struct reac_mode *mode;
	switch (sel & 3) {
	case 0:  mode = &REAC_MODE_44K1; break;
	case 1:  mode = &REAC_MODE_48K;  break;
	case 2:  mode = &REAC_MODE_96K;  break;
	default: mode = &custom;         break;
	}

	const uint8_t *body;
	size_t len = fuzz_take(&in, in.n, &body);
	uint8_t *frame = fuzz_dup(body, len);

	/* The helpers every ingest door calls first. */
	int is_reac = reac_frame_is_reac(frame, len);
	if (is_reac != (len >= 14 && frame[12] == 0x88 && frame[13] == 0x19))
		abort();
	size_t clean = reac_frame_clean_len(len);
	/* Only the +2 FCS residue is ever stripped, and only off a length that is a
	 * legal geometry once it is gone. */
	if (clean != len &&
	    (clean + 2 != len || clean < REAC_UPSTREAM_OVERHEAD ||
	     (clean - REAC_UPSTREAM_OVERHEAD) % REAC_UPSTREAM_BYTES_PER_CH != 0))
		abort();

	fuzz_downstream(frame, len, mode);
	fuzz_upstream(frame, len);
	free(frame);
	return 0;
}
