// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// Braid layout per norihiro/obs-h8819-source convert_to_pcm24lep (GPL-3.0-or-later)
// and per-gron/reacdriver MbufUtils (GPL-3.0).

/* reac_encode — put REAC audio ON the wire: the ENCODE half of the layout.
 *
 * libreac already owned the byte map (<reac/reac_braid.h>), the sample pair
 * (<reac/reac_sample.h>) and the decoders (<reac/reac_decode.h>,
 * <reac/reac_upstream.h>). This header completes the picture: the frame
 * builders that lay audio into a REAC frame. Encoding a frame is a statement
 * about the WIRE FORMAT — the same statement decoding makes, read backwards —
 * so it belongs beside the oracle it uses, not in a consumer. Moved here from
 * reac-pw (its src/reac_tx.c reac_tx_build, and the static place_braided_audio
 * of its src/reac_ctrl.c) 2026-07-29; reac-pw now calls these and keeps only
 * what is genuinely its own: AF_PACKET emission, the SCHED_FIFO pacer, the
 * master/slave FSM and the control-block/checksum protocol state.
 *
 * WHAT STAYED IN reac-pw, and why it is not an oversight:
 *   - reac_tx_emit + the AF_PACKET socket — IO, and libreac is IO-free;
 *   - reac_ctrl_build_* (cold-connect, config-announce, upstream/flood FILLER,
 *     head-amp) — those frames are the box/mixer HANDSHAKE. Their 32-byte
 *     control block, its two nested checksums and the box-model matrix are
 *     protocol STATE tied to the role FSM, not wire layout. They call
 *     reac_braid_encode() for their audio region and own the rest.
 *   - reac_master_stamp / the counter cadence — role-dependent protocol.
 * There is therefore no whole-frame UPSTREAM builder here: on a real box every
 * box->master frame that carries audio is also a control frame, so the only
 * separable, layout-pure part of the upstream emit is its audio region — which
 * is exactly reac_braid_encode(), and which is the SAME braid the downstream
 * uses. Inventing a reac_upstream_build() would have meant importing the
 * handshake, so it was deliberately not done.
 *
 * Both entry points are pure: caller-provided output buffer, no allocation, no
 * IO, no globals — safe to call from a real-time thread (reac-pw calls
 * reac_downstream_build from its PipeWire sink's process callback).
 *
 * Consumers need libm: reac_f32_to_s24le() rounds with lrintf().
 */
#ifndef LIBREAC_REAC_ENCODE_H
#define LIBREAC_REAC_ENCODE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lay planar float audio into a BRAIDED audio region — the exact inverse of
 * reac_upstream_decode()'s un-braid, and the same byte map reac_braid_pos()
 * defines. This is the REAC wire layout in BOTH directions (see
 * <reac/reac_braid.h> for the byte map and its full evidence trail:
 * reacdriver's to-device wordswap16 of BE s24, obs-h8819's
 * convert_to_pcm24lep, and the FreeREAC rig's S-1608/S-0808/S-4000 returns).
 *
 *   audio   points at the frame's audio region — frame + REAC_AUDIO_OFFSET
 *           (50). Must hold n_ch * 12 * 3 bytes.
 *   n_ch    the FRAME's channel width: 40 for the downstream broadcast, the
 *           box's even input count (2..38) for an upstream return. Every one
 *           of these n_ch channels is written, so the region is fully defined
 *           on return — the braid is a bijection over it.
 *   planar  planar[ch][s], ch < n_src. A NULL planar, a NULL plane, or a
 *           channel index >= n_src encodes DIGITAL SILENCE for that channel
 *           (the frame width is a wire fact; the caller need not supply 40
 *           planes to emit a legal 40-channel frame).
 *   n_src   how many planes `planar` actually has.
 *   ns      samples available per plane. Clamped to REAC_SAMPLES_PER_PKT (12):
 *           a REAC frame carries exactly 12 samples per channel at every
 *           sample rate (the rate is the PACKET rate). Time samples beyond ns
 *           are left as the caller found them — callers that need silence
 *           there zero the buffer first, as both reac-pw builders do.
 *
 * Out-of-range floats clamp to the 24-bit signed range (reac_sample.h), so a
 * hot mix cannot wrap round to the opposite polarity on the wire. */
void reac_braid_encode(uint8_t *audio, int n_ch,
                       float *const *planar, int n_src, int ns);

/* Build one complete DOWNSTREAM (master -> fabric) broadcast frame into
 * out[REAC_FRAME_BYTES]. Returns REAC_FRAME_BYTES (1492).
 *
 * The frame, byte for byte:
 *   [0:6]     broadcast destination — the master's program goes to the fabric;
 *   [6:12]    `src`, the emitter's MAC (Roland OUI on the real gear);
 *   [12:14]   EtherType 0x8819;
 *   [14:16]   `counter`, u16 LITTLE-endian, free-running, wraps at 16 bits;
 *   [16:18]   type 0x0000 — FILLER, which carries audio and is checksum-exempt;
 *   [18:50]   the 32-byte control block, left ZERO here. A master role
 *             overwrites it on every frame it stamps, FILLER included (a real
 *             desk does not leave it zero on the wire either); that stamping
 *             is protocol state and stays in the caller;
 *   [50:1490] 1440 B of audio: 40 ch x 12 samples x 3 B, in the BRAID. The
 *             offset is exactly 50 — a golden offset scan showed real desk
 *             program decodes as noise at every other offset;
 *   [1490:1492] the 0xC2 0xEA end marker.
 *
 * `planar`/`nch`/`ns` are as reac_braid_encode's planar/n_src/ns: channels
 * beyond nch (and NULL planes) are silent, so a 2-channel source still emits a
 * legal 40-channel frame. The whole buffer is zeroed first, so a short ns
 * leaves silence rather than stale audio.
 *
 * History worth keeping: this encode was braid (reac-pw 895afb9, correct), then
 * regressed to plain-LE when a full-scale clipped program pile plus a
 * box-in1->out8 loopback was misattributed to the braid as a "burst". The plain
 * encode only ATTENUATED the garbage to a -42 dBFS hash ("right level, garbage
 * content") — it fixed nothing. The A/B env override that kept plain selectable
 * was removed once the Stage B listen test settled it. Do not reintroduce a
 * layout switch here: there is one wire format, and reac_braid_pos() is it. */
int reac_downstream_build(uint8_t *out, float *const *planar, int nch, int ns,
                          uint16_t counter, const uint8_t src[6]);

#ifdef __cplusplus
}
#endif

#endif /* LIBREAC_REAC_ENCODE_H */
