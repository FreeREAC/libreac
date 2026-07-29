// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// Braid layout per norihiro/obs-h8819-source convert_to_pcm24lep (GPL-3.0-or-later)
// and per-gron/reacdriver MbufUtils (GPL-3.0).

/* reac_braid — the REAC audio-region byte layout (the single layout oracle).
 *
 * A REAC frame's audio region (frame[50:]) packs 12 time-samples of n_ch
 * channels of s24 PCM in the channel-pair byte BRAID: per time sample each
 * channel PAIR (2k, 2k+1) occupies one 6-byte group at (s*n_ch + 2k)*3; within
 * the group the even channel's s24 LE bytes (lo,mid,hi) sit at
 * group[3],group[0],group[1] and the odd channel's at group[4],group[5],group[2].
 * Equivalently: a 16-bit-word byte-swap of the pair packed as big-endian s24
 * (reacdriver's to-device conversion). The mapping is bijective over the whole
 * audio region (pinned by libreac tests/test_braid.c and reac-pw's TX suite).
 *
 * This braid is the REAC wire format in BOTH directions, confirmed by three
 * independent sources plus rig goldens:
 *   - reacdriver (per-gron, the macOS REAC driver): its to-device conversion is
 *     byte-identical to this braid (MbufUtils.cpp);
 *   - obs-h8819 (norihiro): convert_to_pcm24lep, developed and LISTENING-
 *     validated against a real Roland M-200i downstream at 48 kHz;
 *   - the FreeREAC rig: the S-1608/S-0808/S-4000 upstream returns decode real
 *     microphones under the braid (reac-pw task #108, upstream fixtures here);
 *   - zoneA/zoneB goldens (a real M-5000's two REAC ports, program audio)
 *     decode at coherence 0.99 / spectral flatness 0.002 under the braid at
 *     audio offset exactly 50, and as noise under every other layout x offset
 *     (reac-pw docs/VALIDATION-PLAN.md Stage B coherence table).
 *
 * There is ONE downstream layout across every mixer generation: the historical
 * "plain LE sample-major" alternative, and the per-generation "M-5000 plain-LE
 * vs M-200/M-300 braid" split that went with it, are refuted — its "coherence
 * 0.999" was a mid-byte lane shift amplifying quiet braided audio 256x into a
 * coherent-looking image. Since 0.5.0 both directions decode this braid by
 * default (reac_decode / reac_upstream_decode) and plain LE survives only as
 * the explicitly named diagnostic reac_decode_plain_le() (see reac_decode.h).
 * Do not add a second copy of this byte map anywhere; consumers (reac-pw,
 * reac-aes67) call this oracle.
 */
#ifndef LIBREAC_REAC_BRAID_H
#define LIBREAC_REAC_BRAID_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Byte positions (pos[0]=lo, pos[1]=mid, pos[2]=hi) of time-sample s, channel
 * ch inside an n_ch-wide braided audio region (offsets relative to the region
 * start, frame[50]). n_ch is the frame's channel width: 40 for the downstream
 * broadcast, the box's even input count (2..38) for an upstream return.
 * Caller guarantees 0 <= ch < n_ch and 0 <= s < 12; the positions returned are
 * then always inside the n_ch*36-byte region. static inline so RT encode/decode
 * paths pay no call cost. */
static inline void reac_braid_pos(int s, int ch, int n_ch, size_t pos[3])
{
	size_t g = (size_t)(s * n_ch + (ch & ~1)) * 3;
	if ((ch & 1) == 0) {
		pos[0] = g + 3; pos[1] = g + 0; pos[2] = g + 1;
	} else {
		pos[0] = g + 4; pos[1] = g + 5; pos[2] = g + 2;
	}
}

#ifdef __cplusplus
}
#endif

#endif /* LIBREAC_REAC_BRAID_H */
