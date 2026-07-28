// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// Portions derived from obs-h8819-source, Copyright (C) 2022 Norihiro Kamae <norihiro@nagater.net> (GPL-3.0-or-later).
// REAC wire framing per github.com/per-gron/reacdriver (GPL-3.0).

#include "reac/reac_decode.h"

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

/* De-interleave the 1440 B audio region into planar 24-bit LE PCM.
 *
 * REAC packs audio sample-major and little-endian: for each of the 12 time-
 * samples all n_channels appear in order, each a 3-byte LE 24-bit value, so
 * channel ch / time-sample s starts at (s*n_channels + ch)*RESOLUTION and the
 * de-interleave is a straight copy.
 *
 * NOTE — the plain-LE layout this decodes is CONTESTED, kept as the
 * diagnostic/legacy downstream path only. The 2026-06-06 on-rig comparison that
 * concluded "plain LE: coherent 0.999; obs-h8819 braid: noise" was later
 * overturned: the zoneA/zoneB goldens (the SAME M-5000's two REAC ports,
 * program audio) decode BRAIDED at coherence 0.99 / spectral flatness 0.002,
 * and the plain "coherence 0.999" was a mid-byte lane shift amplifying quiet
 * braided audio 256x into a coherent-looking image (reac-pw
 * docs/VALIDATION-PLAN.md Stage B coherence table; three independent sources —
 * reacdriver, obs-h8819, the FreeREAC rig #108 — back the braid as the wire
 * format in both directions). The braid oracle lives in <reac/reac_braid.h>
 * (box upstream decode: <reac/reac_upstream.h>). This plain path is retained
 * unchanged so existing consumers (reac-aes67) keep byte-identical behavior
 * until re-verified on the rig; do NOT extend new code from this layout.
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
