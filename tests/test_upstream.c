// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test: the UPSTREAM (stagebox->master) frame decode.
 *
 * Fixtures are REAL captured frames from the rig (reac-captures zoneA-48k /
 * zoneB-48k, MAC-sanitized to the stand-in 00:40:ab:c4:80:f6 per that repo's
 * convention): an S-1608 16-ch 628 B return, an S-0808 8-ch 340 B return, and
 * two consecutive S-4000 32-ch 1206 B returns, i.e. 1204 B frames the capture
 * left 2 bytes of FCS on (matrix-m200-s4000
 * 2026-07-24; 1206 = 52 + 32*36 + the +2 CRC trailer after the end marker,
 * with a 1204 B trailerless variant on the same wire).
 * The expected planar PCM tables were produced by the capture-side analysis
 * that resolved the layout (2026-07-10, task #108): the upstream audio region
 * uses the obs-h8819 even/odd channel-pair byte BRAID (even ch = group bytes
 * 3,0,1; odd ch = 4,5,2 as LE lo/mid/hi), NOT the M-5000 downstream's plain
 * LE sample-major layout. Verified against the loud wired capture: the music
 * channel decodes at lag1 autocorrelation +0.998 under the braid vs noise
 * under plain LE.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <reac/reac.h>
#include <reac/reac_upstream.h>

#include "upstream_fixtures.inc"

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static int32_t s24_at(const uint8_t *out, int nch, int ch, int s)
{
	(void)nch;
	const uint8_t *p = out + (size_t)(ch * REAC_SAMPLES_PER_PKT + s) * 3;
	int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
	if (v & 0x00800000)
		v |= ~0x00FFFFFF;
	return v;
}

int main(void)
{
	/* 1. shape: nch from frame length (len = 52 + nch*36) */
	CHK(reac_upstream_channels(628) == 16);  /* S-1608 */
	CHK(reac_upstream_channels(340) == 8);   /* S-0808 */
	CHK(reac_upstream_channels(1492) == 40); /* a 40-wide box's return: a box may fill the
	                                          * fabric (ruling 2026-09-25); width does not
	                                          * say desk — direction and role do */
	CHK(reac_upstream_channels(1528) == -1); /* 42: wider than any box */
	CHK(reac_upstream_channels(627) == -1);
	CHK(reac_upstream_channels(629) == -1);
	CHK(reac_upstream_channels(52) == -1);   /* nch 0 */
	CHK(reac_upstream_channels(160) == -1);  /* odd nch (3): braid needs channel pairs */
	CHK(reac_upstream_channels(0) == -1);
	CHK(reac_upstream_channels(14) == -1);

	/* 2. decode the captured 16-ch frame: header + counter + full PCM table */
	uint8_t out[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION];
	int ns = reac_upstream_decode(UP16, sizeof UP16, out);
	CHK(ns == REAC_SAMPLES_PER_PKT);
	CHK(reac_frame_counter(UP16) == 0xd9b1); /* same byte-14/15 LE counter as downstream */
	int bad = 0;
	for (int ch = 0; ch < 16; ch++)
		for (int s = 0; s < 12; s++)
			if (s24_at(out, 16, ch, s) != UP16_PCM[ch][s])
				bad++;
	CHK(bad == 0);

	/* 3. S-4000 32-ch returns, captured at 1206 B = 52 + 32*36 + the capture
	 * path's +2. THE FIXTURES ARE AS CAPTURED AND THE TEST IS THE READER: it
	 * strips with reac_frame_clean_len() — ingest's rule — and hands the parser
	 * 1204. The parser REFUSES 1206 outright (since 2026-09-21: the residue is
	 * the tap's, never the wire's, and a parser that stripped it would hide a
	 * reader that forgot), and the decode of the same real frame at 1204 B is
	 * byte-equal to the PCM tables, so nothing was lost by the strip. */
	CHK(reac_upstream_channels(1206) == -1); /* the capture's +2, unstripped */
	CHK(reac_upstream_channels(1204) == 32); /* the frame itself */
	CHK(reac_upstream_channels(1205) == -1);
	CHK(reac_frame_clean_len(sizeof UP32A) == 1204); /* what the reader hands over */
	CHK(reac_upstream_decode(UP32A, sizeof UP32A, out) == -1); /* unstripped: refused */
	ns = reac_upstream_decode(UP32A, reac_frame_clean_len(sizeof UP32A), out);
	CHK(ns == REAC_SAMPLES_PER_PKT);
	CHK(reac_frame_counter(UP32A) == 0xff9c);
	bad = 0;
	for (int ch = 0; ch < 32; ch++)
		for (int s = 0; s < 12; s++)
			if (s24_at(out, 32, ch, s) != UP32A_PCM[ch][s])
				bad++;
	CHK(bad == 0);
	/* the residue bytes never reach the audio: the same real frame read at its
	 * 1204 B length, with the two bytes sliced off by hand rather than by the
	 * reader's rule, yields the identical planar PCM */
	uint8_t out2[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION];
	CHK(reac_upstream_decode(UP32A, 1204, out2) == REAC_SAMPLES_PER_PKT);
	CHK(memcmp(out, out2, (size_t)32 * 12 * 3) == 0);
	/* the second consecutive frame (counter +1) pins the per-frame stability */
	ns = reac_upstream_decode(UP32B, reac_frame_clean_len(sizeof UP32B), out);
	CHK(ns == REAC_SAMPLES_PER_PKT);
	CHK(reac_frame_counter(UP32B) == 0xff9d);
	bad = 0;
	for (int ch = 0; ch < 32; ch++)
		for (int s = 0; s < 12; s++)
			if (s24_at(out, 32, ch, s) != UP32B_PCM[ch][s])
				bad++;
	CHK(bad == 0);

	/* 4. decode the captured 8-ch frame */
	ns = reac_upstream_decode(UP8, sizeof UP8, out);
	CHK(ns == REAC_SAMPLES_PER_PKT);
	bad = 0;
	for (int ch = 0; ch < 8; ch++)
		for (int s = 0; s < 12; s++)
			if (s24_at(out, 8, ch, s) != UP8_PCM[ch][s])
				bad++;
	CHK(bad == 0);

	/* 5. validation rejects */
	uint8_t f[628];
	memcpy(f, UP16, sizeof f);
	f[12] = 0x08; /* not 0x8819 */
	CHK(reac_upstream_decode(f, sizeof f, out) == -1);
	memcpy(f, UP16, sizeof f);
	f[626] = 0x00; /* broken end marker */
	CHK(reac_upstream_decode(f, sizeof f, out) == -1);
	memcpy(f, UP16, sizeof f);
	CHK(reac_upstream_decode(f, 627, out) == -1);          /* truncated */
	CHK(reac_upstream_decode(NULL, 628, out) == -1);
	CHK(reac_upstream_decode(f, sizeof f, NULL) == -1);
	/* a residue length is refused at EVERY width, like any other off-law length:
	 * 52 + n*36 + 2 is a capture artifact and the reader owns it */
	CHK(reac_upstream_decode(f, 630, out) == -1);
	CHK(reac_upstream_channels(342) == -1);
	CHK(reac_upstream_channels(630) == -1);
	CHK(reac_upstream_channels(1494) == -1);

	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("OK: upstream decode — 16-ch 628 B + 8-ch 340 B + 32-ch 1204 B (captured at 1206, the "
	       "reader strips the tap's +2 and the parser refuses it unstripped), braid layout, full "
	       "PCM match, shape/validation rejects\n");
	return 0;
}
