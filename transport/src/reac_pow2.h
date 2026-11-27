// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#ifndef REAC_POW2_H
#define REAC_POW2_H

#include <stdint.h>

/* The power-of-two ceiling both rings size themselves by: at least 2, and 0 when the
 * ceiling would pass UINT32_MAX (v above 2^31), which each caller refuses before it
 * allocates — reac_ring_init and reac_frame_ring_init each say why. ONE home for the
 * two rings (the audio ring in reac_ring.c, the frame ring in reac_pacer.c), which held
 * a copy each; tests/conformance-declared-once.sh ARM 8 keeps it so. */
static inline uint32_t reac_next_pow2(uint32_t v)
{
	if (v < 2)
		return 2;
	v--;
	v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
	return v + 1;
}

#endif
