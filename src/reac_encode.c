// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// Braid layout per norihiro/obs-h8819-source convert_to_pcm24lep (GPL-3.0-or-later)
// and per-gron/reacdriver MbufUtils (GPL-3.0).
//
// The encode half of the REAC audio layout. Moved from reac-pw 2026-07-29 (its
// src/reac_tx.c reac_tx_build + the static place_braided_audio of its
// src/reac_ctrl.c, which were the same braid loop written twice) so the wire
// format has ONE home in both directions. The move was proven byte-identical:
// a differential corpus (every box width, both roles' builders, counters
// including the 16-bit wrap, clipping and NULL-plane inputs) built by the
// pre-move code and by this file compares equal, and the digest of that corpus
// is pinned as a golden in tests/test_encode.c here and in reac-pw's TX suite.

#include "reac/reac_encode.h"

#include "reac/reac.h"         /* REAC_FRAME_BYTES, _AUDIO_OFFSET, _HDR_COUNTER_OFF, ... */
#include "reac/reac_braid.h"   /* reac_braid_pos — the layout oracle */
#include "reac/reac_sample.h"  /* reac_f32_to_s24le — the s24 half of the codec pair */

#include <string.h>

void reac_braid_encode(uint8_t *audio, int n_ch,
                       float *const *planar, int n_src, int ns)
{
	/* A REAC frame carries exactly 12 samples per channel at every sample rate
	 * — the rate rides on the PACKET rate, not on the frame size. Clamp rather
	 * than trust the caller's ns so a graph quantum longer than a frame cannot
	 * run off the end of the audio region. */
	int frames = ns < REAC_SAMPLES_PER_PKT ? ns : REAC_SAMPLES_PER_PKT;

	for (int s = 0; s < frames; s++) {
		for (int ch = 0; ch < n_ch; ch++) {
			/* Frame width is a wire fact; how many planes the caller has is
			 * not. A missing plane is digital silence, which is what a real
			 * desk sends on an unpatched channel. */
			float v = (planar && ch < n_src && planar[ch]) ? planar[ch][s] : 0.0f;
			uint8_t b[3];
			size_t pos[3];

			reac_f32_to_s24le(v, b);      /* lo, mid, hi — clamped, lrintf-rounded */
			reac_braid_pos(s, ch, n_ch, pos);
			audio[pos[0]] = b[0];
			audio[pos[1]] = b[1];
			audio[pos[2]] = b[2];
		}
	}
}

int reac_downstream_build(uint8_t *out, float *const *planar, int nch, int ns,
                          uint16_t counter, const uint8_t src[6])
{
	memset(out, 0, REAC_FRAME_BYTES);

	/* L2: broadcast dst (the master's program goes to the whole fabric), our
	 * src, EtherType 0x8819. */
	memset(out, 0xFF, 6);
	memcpy(out + 6, src, 6);
	out[12] = (REAC_ETHERTYPE >> 8) & 0xFF;   /* 0x88 */
	out[13] = REAC_ETHERTYPE & 0xFF;          /* 0x19 */

	/* u16-LE sequence counter at byte 14; type 0x0000 (FILLER carries audio and
	 * is checksum-exempt) + the 32-byte control block at 18..49 stay zero — the
	 * caller's master role stamps that block afterwards if it has one. */
	out[REAC_HDR_COUNTER_OFF]     = (uint8_t)(counter & 0xFF);
	out[REAC_HDR_COUNTER_OFF + 1] = (uint8_t)((counter >> 8) & 0xFF);

	/* Audio at offset exactly 50: the full 40-channel downstream width, in the
	 * braid. Channels beyond nch (and NULL planes) are silent; the memset above
	 * means a short ns leaves silence rather than stale audio. */
	reac_braid_encode(out + REAC_AUDIO_OFFSET, REAC_MAX_CHANNELS,
	                  planar, nch, ns);

	out[REAC_FRAME_BYTES - 2] = REAC_END_MARKER_0;  /* 0xC2 */
	out[REAC_FRAME_BYTES - 1] = REAC_END_MARKER_1;  /* 0xEA */
	return REAC_FRAME_BYTES;
}
