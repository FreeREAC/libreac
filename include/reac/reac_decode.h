// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
// Portions derived from obs-h8819-source, Copyright (C) 2022 Norihiro Kamae <norihiro@nagater.net> (GPL-3.0-or-later).

/* Pure REAC DOWNSTREAM frame decode: raw 1492-byte master broadcast ->
 * interleaved 24-bit PCM. No I/O, no allocation, RT-safe.
 *
 * *** BEHAVIOUR CHANGE IN 0.5.0 — reac_decode() now reads the BRAID. ***
 *
 * Up to 0.4.0 reac_decode() read the audio region as PLAIN LE sample-major
 * while reac_downstream_build() (<reac/reac_encode.h>) wrote the channel-pair
 * braid, so the library could not read back a frame it had just built: on a
 * 1492 B frame of its own making, 0 of 480 samples agreed (libreac#13). There
 * is ONE downstream layout for every mixer generation and it is the braid — the
 * per-generation "M-5000 plain-LE vs M-200/M-300 braid" split that once kept
 * this path as the default is refuted, not open. See <reac/reac_braid.h> for
 * the byte map and the full evidence trail.
 *
 * So the DEFAULT is now correct and every existing caller became correct
 * without a source change. Both directions decode the same braid through the
 * same oracle: reac_decode() here (downstream, 40 ch), reac_upstream_decode()
 * in <reac/reac_upstream.h> (box return, box-width sized).
 *
 * The plain-LE layout survives under its own explicit name,
 * reac_decode_plain_le(), byte-identical to the pre-0.5.0 reac_decode(). It is
 * a DIAGNOSTIC: it reads historical captures decoded that way, and it is how
 * the mid-byte lane shift that once made plain-LE look coherent is reproduced.
 * It is not a layout the wire ever carried. */
#ifndef REAC_DECODE_H
#define REAC_DECODE_H

#include <stdint.h>
#include <stddef.h>
#include <reac/reac.h>

struct reac_frame {
	uint16_t counter;   /* l2_counter, little-endian */
	int valid;          /* 1 if ethertype + length + end marker checked out */
};

/* Validate a raw REAC frame and extract its l2_counter.
 * raw/len is the full ethernet frame (starting at the dest MAC).
 * Returns a reac_frame with valid=1 only if it is a well-formed 0x8819 frame
 * of the expected length with the 0xC2 0xEA end marker. */
struct reac_frame reac_frame_inspect(const uint8_t *raw, size_t len,
                                     const struct reac_mode *mode);

/* Decode the audio region of a validated downstream frame into planar 24-bit LE
 * PCM, un-braiding the channel pairs — the exact inverse of what
 * reac_downstream_build() lays down.
 *
 * out must hold mode->n_channels * mode->samples_per_pkt samples; each sample
 * is written as 3 bytes (little-endian, signed 24-bit), grouped by channel
 * (channel 0's samples first, then channel 1's, ...). i.e. out is planar:
 *   out[(ch*samples_per_pkt + s)*3 + {0,1,2}]
 *
 * raw/len is the full ethernet frame; the audio region begins at
 * REAC_L2_HEADER_LEN. Returns the number of samples written per channel, or
 * -1 on a malformed frame, or on a mode descriptor whose channel width is odd
 * (the braid packs channel PAIRS) or whose geometry does not fit the 1440-byte
 * audio region. The shipped REAC_MODE_* descriptors are all 40 x 12. */
int reac_decode(const uint8_t *raw, size_t len, const struct reac_mode *mode,
                uint8_t *out);

/* DIAGNOSTIC ONLY — the pre-0.5.0 reac_decode(), unchanged.
 *
 * Reads the audio region as PLAIN LE sample-major: channel ch / time-sample s
 * starting at (s*n_channels + ch)*REAC_RESOLUTION, a straight copy. That is not
 * the wire format; a real frame decoded this way is the sign-extension smear
 * the braid evidence trail describes. Kept reachable for exactly two jobs:
 * reading back historical captures that were stored under this layout, and
 * reproducing the mid-byte lane shift that amplified quiet braided audio 256x
 * into the "coherence 0.999" reading which once argued for it.
 *
 * Same arguments, same planar output shape and same return contract as
 * reac_decode(), minus the braid geometry checks (a linear index needs none).
 * Do not build new code on this layout. */
int reac_decode_plain_le(const uint8_t *raw, size_t len,
                         const struct reac_mode *mode, uint8_t *out);

#endif /* REAC_DECODE_H */
