// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_upstream — decode the UPSTREAM (stagebox -> master) audio frame.
 *
 * A box's return frame shares the downstream's envelope — 14 B eth + u16 LE
 * counter @14 + 2 B type + 32 B descriptor area (16 x `00 7a` slot words,
 * regardless of box width) = 50 B header, then the audio region, then the
 * 0xC2 0xEA end marker — but is BOX-WIDTH sized:
 *
 *     frame_len = 52 + n_channels * 36        (36 = 12 samples x 3 B)
 *     S-1608 -> 16 ch -> 628 B;  S-0808 -> 8 ch -> 340 B
 *
 * and its audio region is NOT the plain LE sample-major layout of
 * reac_decode(): it uses the channel-pair byte BRAID — the REAC wire layout,
 * see <reac/reac_braid.h> for the byte map and the full evidence trail.
 *
 * Resolved 2026-07-10 (reac-pw task #108) against the rig captures in
 * reac-captures: under the braid the loud wired capture's music channel decodes
 * at lag1 autocorrelation +0.998 and every idle channel collapses to the mic
 * noise floor; under plain LE every channel reads sign-extension garbage (the
 * historical "45k RMS on all slots" smear). Channel order is plain ascending
 * (input N = wire channel N-1, 0-based) — no further FPGA scramble.
 *
 * Like the downstream frame, the upstream is rate-invariant: always 12
 * samples per frame, the sample rate carried by the packet rate. OHRCA-path
 * boxes (S-4000 on an M-5000/M-480 fabric) append the +2 CRC trailer
 * (see REAC_FRAME_BYTES_OHRCA in <reac/reac.h>); both entry points below
 * accept the trailered and the clean length alike.
 *
 * Moved verbatim from reac-pw (its src/reac_upstream.{h,c}) 2026-07-28 so the
 * layout has one home; the reac-pw captured-frame fixtures moved with it
 * (tests/upstream_fixtures.inc, also still pinned from reac-pw's suite). */
#ifndef LIBREAC_REAC_UPSTREAM_H
#define LIBREAC_REAC_UPSTREAM_H

#include <stdint.h>
#include <stddef.h>

#include <reac/reac.h>  /* REAC_UPSTREAM_OVERHEAD / _BYTES_PER_CH */

#ifdef __cplusplus
extern "C" {
#endif

/* Channel count carried by an upstream frame of `len` bytes, derived from the
 * frame size (an OHRCA +2 CRC trailer, if present, is stripped first). Returns
 * -1 unless clean len = 52 + nch*36 with nch even (the braid packs channel
 * pairs), 2 <= nch < 40. The 40-ch solution (1492 B) is the DOWNSTREAM
 * broadcast, never a box return, and is rejected. */
int reac_upstream_channels(size_t len);

/* Decode a validated upstream frame's audio region into planar 24-bit LE PCM,
 * un-braiding the channel pairs. out must hold nch * 12 samples x 3 B (nch
 * from reac_upstream_channels; REAC_MAX_CHANNELS*12*3 always suffices) and is
 * planar like reac_decode's output: out[(ch*12 + s)*3 + {0,1,2}].
 *
 * raw/len is the full ethernet frame. Validates the 0x8819 ethertype, the
 * frame shape and the end marker. Returns the samples written per channel
 * (12), or -1 on a malformed frame. */
int reac_upstream_decode(const uint8_t *raw, size_t len, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LIBREAC_REAC_UPSTREAM_H */
