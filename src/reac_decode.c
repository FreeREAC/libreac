// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// Portions derived from obs-h8819-source, Copyright (C) 2022 Norihiro Kamae <norihiro@nagater.net> (GPL-3.0-or-later).
// REAC wire framing per github.com/per-gron/reacdriver (GPL-3.0).

#include "reac/reac_decode.h"

#include "reac/reac_braid.h"  /* reac_braid_pos — the layout oracle */

/* The REAC mode descriptors (REAC_MODE_44K1 / 48K / 96K) and the geometry
 * constants now live in libreac — see <reac/reac.h>. */

struct reac_frame reac_frame_inspect(const uint8_t *raw, size_t len,
                                     const struct reac_mode *mode)
{
	(void)mode;
	struct reac_frame f = { 0, 0 };
	if (len != (size_t)REAC_FRAME_BYTES)
		return f;
	/* ethertype at bytes 12-13 (after dst6+src6) */
	if (raw[12] != 0x88 || raw[13] != 0x19)
		return f;
	/* end marker */
	if (raw[len - 2] != REAC_END_MARKER_0 || raw[len - 1] != REAC_END_MARKER_1)
		return f;
	f.counter = (uint16_t)(raw[14] | (raw[15] << 8)); /* l2_counter, LE */
	f.valid = 1;
	return f;
}

/* De-interleave the 1440 B downstream audio region into planar 24-bit LE PCM,
 * un-braiding the channel pairs.
 *
 * This is the DEFAULT since 0.5.0 and it is the inverse of the builder next
 * door: reac_downstream_build() writes through reac_braid_encode(), this reads
 * through the same reac_braid_pos() oracle, so the library can read back what
 * it writes. Before 0.5.0 it read PLAIN LE sample-major and could not: 0 of 480
 * samples of a self-built frame agreed (libreac#13).
 *
 * The braid is the wire format in BOTH directions and for every mixer
 * generation — the per-generation "M-5000 plain-LE vs M-200/M-300 braid" split
 * that kept plain-LE the default here is refuted, not open, so this file no
 * longer carries a "until a rig re-verify flips the default" gate. What settled
 * it: the zoneA/zoneB goldens (the SAME M-5000's two REAC ports, program audio)
 * decode BRAIDED at coherence 0.99 / spectral flatness 0.002 at audio offset
 * exactly 50, and as noise under every other layout x offset; a real box played
 * braid-encoded downstream correctly in an A/B listen, where plain-LE on the
 * same path only attenuated the same garbage to a -42 dBFS hash; three
 * independent sources agree (reacdriver's to-device conversion, obs-h8819's
 * convert_to_pcm24lep, the FreeREAC rig's S-1608/S-0808/S-4000 returns); and
 * plain-LE's "coherence 0.999" was a mid-byte lane shift amplifying quiet
 * braided audio 256x into a coherent-looking image. Full trail in
 * <reac/reac_braid.h>, which is the one place the byte map lives.
 */
int reac_decode(const uint8_t *raw, size_t len, const struct reac_mode *mode,
                uint8_t *out)
{
	struct reac_frame f = reac_frame_inspect(raw, len, mode);
	if (!f.valid)
		return -1;

	const uint8_t *audio = raw + REAC_L2_HEADER_LEN;
	const int nch = mode->n_channels;
	const int ns = mode->samples_per_pkt;

	/* The braid packs channel PAIRS, so an odd width has no byte map; and the
	 * positions it returns are only guaranteed inside the region when the
	 * descriptor's geometry is the region's. The plain-LE path below indexed
	 * linearly and needed neither check. */
	if (nch <= 0 || (nch & 1) || ns <= 0 ||
	    (size_t)nch * (size_t)ns * REAC_RESOLUTION > (size_t)REAC_AUDIO_BYTES)
		return -1;

	uint8_t *dptr = out;
	for (int ch = 0; ch < nch; ch++) {
		for (int s = 0; s < ns; s++) {
			size_t pos[3];
			reac_braid_pos(s, ch, nch, pos); /* the layout oracle */
			*dptr++ = audio[pos[0]];  /* lo  */
			*dptr++ = audio[pos[1]];  /* mid */
			*dptr++ = audio[pos[2]];  /* hi  */
		}
	}
	return ns;
}

/* The PLAIN LE sample-major read: channel ch / time-sample s starts at
 * (s*n_channels + ch)*RESOLUTION and the de-interleave is a straight copy.
 *
 * This was reac_decode() up to 0.4.0 and is kept byte-identical under its own
 * name, deliberately: a layout that never was on the wire is still the layout
 * some historical captures were decoded and stored under, and it is how the
 * mid-byte lane shift behind the old "coherence 0.999" reading is reproduced.
 * Diagnostic only — see the header. Do NOT extend new code from this layout.
 */
int reac_decode_plain_le(const uint8_t *raw, size_t len,
                         const struct reac_mode *mode, uint8_t *out)
{
	struct reac_frame f = reac_frame_inspect(raw, len, mode);
	if (!f.valid)
		return -1;

	const uint8_t *audio = raw + REAC_L2_HEADER_LEN;
	const int nch = mode->n_channels;
	const int ns = mode->samples_per_pkt;

	uint8_t *dptr = out;
	for (int ch = 0; ch < nch; ch++) {
		for (int s = 0; s < ns; s++) {
			const uint8_t *sptr = audio + (size_t)(s * nch + ch) * REAC_RESOLUTION;
			*dptr++ = sptr[0];
			*dptr++ = sptr[1];
			*dptr++ = sptr[2];
		}
	}
	return ns;
}
