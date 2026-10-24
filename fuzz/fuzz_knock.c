// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libFuzzer harness: THE KNOCK (src/reac_knock.c), the proof that a wire carries no
 * master and may therefore be driven.
 *
 * Its only input from the wire is WHEN a frame was heard. The harness feeds it a
 * clock and a pattern of frames chosen by the input and checks the safety argument
 * reac_knock.h makes: a master fills every audio slot, so the licence to drive may
 * be granted only after REAC_KNOCK_LISTEN_NS in which nothing at all was heard.
 *
 * Input:  [0..3] u32 LE: the start time in units of 2^20 ns
 *         then events, at most 4096, each [ev][u16 LE dt]: the clock advances by
 *         dt << (ev >> 4 & 7) ns (so steps run from nanoseconds to seconds), then
 *           ev & 1   a frame is heard (reac_knock_heard)
 *           ev & 2   the caller steps the knock (reac_knock_step)
 *         The clock is monotonic, like the CLOCK_MONOTONIC the daemon passes in.
 *
 * Properties, each an abort():
 *   - DRIVE comes at most once in a knock's life, and `granted` says so;
 *   - DRIVE comes only at a time at least REAC_KNOCK_LISTEN_NS after the wire came
 *     up AND after the last frame heard on it;
 *   - and it is not withheld: a step at or after that time, with nothing heard
 *     since and no licence yet granted, drives.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <reac/reac_knock.h>

#include "fuzz.h"

#define MAX_EVENTS 4096

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size >= FUZZ_INPUT_MAX)
		return 0;
	struct fuzz_in in = fuzz_in(data, size);
	uint64_t now = (uint64_t)fuzz_u32(&in) << 20;
	struct reac_knock k;
	reac_knock_init(&k, now);

	/* The harness's own record of the wire: when it came up, when it was last heard. */
	uint64_t quiet_since = now;
	int drives = 0;

	for (int i = 0; i < MAX_EVENTS && in.n; i++) {
		uint8_t ev = fuzz_u8(&in);
		now += (uint64_t)fuzz_u16(&in) << (ev >> 4 & 7);
		if (ev & 1) {
			reac_knock_heard(&k, now);
			if (now > quiet_since)
				quiet_since = now;
		}
		if (ev & 2) {
			enum reac_knock_act a = reac_knock_step(&k, now);
			if (a != REAC_KNOCK_ACT_NONE && a != REAC_KNOCK_ACT_DRIVE)
				abort();
			if (a == REAC_KNOCK_ACT_DRIVE) {
				/* Once, and only after a whole window of silence. */
				if (++drives > 1 || !k.granted)
					abort();
				if (now < quiet_since || now - quiet_since < REAC_KNOCK_LISTEN_NS)
					abort();
			} else if (!drives && now >= quiet_since &&
			           now - quiet_since >= REAC_KNOCK_LISTEN_NS &&
			           (ev & 1) == 0) {
				/* A silent window closed and nothing was heard this instant: the
				 * licence is due, and withholding it is the latch the re-opening
				 * cancellation exists to prevent. */
				abort();
			}
		}
		if (k.granted && !drives)
			abort();   /* granted without the caller ever being told */
	}
	return 0;
}
