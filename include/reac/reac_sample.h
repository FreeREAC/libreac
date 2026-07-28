// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_sample — the s24-LE <-> float sample conversion pair.
 *
 * REAC carries signed 24-bit LE PCM; the PipeWire/graph side works in
 * normalized float. These two inlines are the ONE conversion pair: they are
 * exact inverses (encode divides/multiplies by the same 2^23, clamps to the
 * 24-bit signed range, rounds with lrintf), so an encode->decode round-trip is
 * the identity up to one ULP of 24-bit quantization. reac-pw's TX/RX and
 * control-plane audio placement all call these; keeping both directions in one
 * header is what guarantees the round-trip contract can never diverge (they
 * were previously duplicated verbatim across three files in reac-pw).
 *
 * Consumers of reac_f32_to_s24le need libm (lrintf). */
#ifndef LIBREAC_REAC_SAMPLE_H
#define LIBREAC_REAC_SAMPLE_H

#include <stdint.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* s24 LE (3 bytes at p) -> normalized float in [-1, 1) */
static inline float reac_s24le_to_f32(const uint8_t *p)
{
	int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
	if (v & 0x00800000)
		v |= ~0x00FFFFFF; /* sign-extend bit 23 */
	return (float)v / 8388608.0f; /* 2^23 */
}

/* normalized float [-1,1) -> 24-bit signed LE (lo,mid,hi at p[0],p[1],p[2]),
 * the exact inverse of reac_s24le_to_f32. Out-of-range input clamps to the
 * 24-bit signed range. */
static inline void reac_f32_to_s24le(float v, uint8_t *p)
{
	float x = v * 8388608.0f;            /* 2^23 */
	if (x > 8388607.0f) x = 8388607.0f;  /* clamp to the 24-bit signed range */
	if (x < -8388608.0f) x = -8388608.0f;
	int32_t s = (int32_t)lrintf(x);
	p[0] = (uint8_t)(s & 0xFF);          /* lo  */
	p[1] = (uint8_t)((s >> 8) & 0xFF);   /* mid */
	p[2] = (uint8_t)((s >> 16) & 0xFF);  /* hi  */
}

#ifdef __cplusplus
}
#endif

#endif /* LIBREAC_REAC_SAMPLE_H */
