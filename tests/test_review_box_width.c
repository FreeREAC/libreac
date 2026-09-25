// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* REVIEW 2026-09-25, finding M4 (docs/audits/2026-09-25-libreac-review.md) —
 * AN OPERATOR QUESTION, NOT A FIX STEP. It is red on purpose and is NOT part of
 * `make test`: the operator's 40-channel experiment row and the spec's even 2..38
 * box width cannot both stand, and which one moves is the operator's call. Run it
 * by hand to see where the question stands:
 *   cc -std=c11 -Iinclude tests/test_review_box_width.c libreac.a -lm -o test_review_box_width
 *   ./test_review_box_width
 *
 * The settled spec (reac-protocol spec/reac.ksy, num_channels and "The role is
 * the geometry"): 40 channels / 1492 B is the MASTER's downstream; a box's
 * upstream return is an even 2..38. libreac states the same law in
 * reac_frame_is_master_downstream() (reac.h), reac_upstream_channels() and
 * reac_braid.h ("the box's even input count (2..38)").
 *
 * Four box-side doors still admit 40, so the library builds box frames its own
 * geometry law reads as a desk:
 *   - the "fr4000" row (in_ch 40) and reac_box_model_upstream_width()'s
 *     `in_ch > REAC_MAX_CHANNELS` bound;
 *   - ctrl_emit_as()'s `n_ch > REAC_MAX_CHANNELS` bound on every box builder;
 *   - reac_boxreg's width_ok() (`nch <= r->fabric`, fabric = 40).
 * A master running libreac classifies such a box as a rival DESK
 * (reac_rival_kind_from_channels(40)) and reac_upstream_decode() refuses its
 * audio. */
#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_upstream.h>
#include <reac/reac_boxreg.h>

#include <stdio.h>

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

int main(void)
{
	static const uint8_t MASTER[6] = { 0x00, 0x40, 0xab, 0x01, 0x02, 0x03 };
	static const uint8_t SRC[6]    = { 0x00, 0x40, 0xab, 0x0f, 0x0e, 0x0d };
	static uint8_t frame[2048];

	/* CONTROL: a real box width builds a frame the library reads back as a box. */
	size_t l16 = reac_ctrl_build_upstream_filler(frame, MASTER, SRC, 1, 16, NULL, 0);
	if (l16 != 628 || reac_upstream_channels(l16) != 16 ||
	    reac_frame_is_master_downstream(l16)) {
		printf("NOT A RESULT: test_review_box_width — the 16-channel control does not "
		       "round-trip\n");
		return 2;
	}

	/* 1. the 40-in experiment row, through the model door. */
	const struct reac_box_model *fr = reac_box_model_by_token("fr4000");
	CHK(fr != NULL);
	if (fr) {
		int w = reac_box_model_upstream_width(fr);
		size_t len = reac_ctrl_build_as(frame, fr, REAC_BOX_BLOCK_CC0014,
		                                MASTER, SRC, 1, NULL, 0);
		CHK(w >= 2 && w <= REAC_MAX_CHANNELS - 2);
		CHK(len == 0 || !reac_frame_is_master_downstream(len));
		CHK(len == 0 || reac_upstream_channels(len) == w);
	}

	/* 2. the width-keyed box builder. */
	size_t l40 = reac_ctrl_build_upstream_filler(frame, MASTER, SRC, 1, 40, NULL, 0);
	CHK(l40 == 0);   /* 40 is not a box width; 1492 B is a desk */

	/* 3. the registry that places a box's return in the fabric. */
	struct reac_boxreg r;
	reac_boxreg_init(&r, 0);
	static const uint8_t BOX[6] = { 0x00, 0x40, 0xab, 0xaa, 0xbb, 0xcc };
	CHK(reac_boxreg_add(&r, BOX, 40) < 0);

	if (fails) {
		fprintf(stderr, "  box-side doors build/accept a 40-channel (1492 B) return, which "
		        "reac_frame_is_master_downstream() reads as a master\n");
		return 1;
	}
	printf("OK: test_review_box_width — no box door emits or accepts the downstream geometry\n");
	return 0;
}
