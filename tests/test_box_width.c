// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* A BOX IS NEVER 40 WIDE. Operator ruling 2026-09-25: "mixer sends 40ch, boxes
 * have their size of ins and outs, always even." A 40-channel frame (1492 B) is
 * the DESK's frame; a box's width, either direction, is an even 2..38
 * (reac_box_width_ok in reac.h; reac-protocol spec/reac.ksy num_channels). This
 * was libreac review 2026-09-25 finding M4: every box door admitted 40, so the
 * library built box frames its own reac_frame_is_master_downstream() read as a
 * desk, and reac_upstream_decode() refused.
 *
 *   CONTROL  a legal even width (16) builds through every box builder, and the
 *            frame reads back as a 16-channel box, not a master;
 *   VERDICT  every box builder, the model door, the registry and the upstream
 *            parser refuse 40, and refuse every odd width 1..41. */
#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_upstream.h>
#include <reac/reac_boxreg.h>

#include <stdio.h>
#include <string.h>

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static const uint8_t MASTER[6] = { 0x00, 0x40, 0xab, 0x01, 0x02, 0x03 };
static const uint8_t SRC[6]    = { 0x00, 0x40, 0xab, 0x0f, 0x0e, 0x0d };
static uint8_t frame[2048];

/* Every box builder that takes a width, at width n; returns how many built. */
static int builders_that_build(int n)
{
	int built = 0;
	built += reac_ctrl_build_box_hb(frame, MASTER, SRC, 1, n) > 0;
	built += reac_ctrl_build_upstream_filler(frame, MASTER, SRC, 1, n, NULL, 0) > 0;
	built += reac_ctrl_build_flood_filler(frame, MASTER, SRC, 1, n, NULL, 0) > 0;
	built += reac_ctrl_build_coldconnect(frame, MASTER, SRC, 1, n, NULL, 0) > 0;
	built += reac_ctrl_build_coldconnect_head(frame, MASTER, SRC, 1, n, NULL, 0) > 0;
	built += reac_ctrl_build_coldconnect_0013(frame, MASTER, SRC, 1, n, NULL, 0) > 0;
	built += reac_ctrl_build_coldconnect_0016(frame, MASTER, SRC, 1, n, NULL, 0) > 0;
	built += reac_ctrl_build_coldconnect_001a(frame, MASTER, SRC, 1, n, NULL, 0) > 0;
	built += reac_ctrl_build_config_announce(frame, MASTER, SRC, 1, n) > 0;
	built += reac_ctrl_build_config_announce_box_master(frame, MASTER, SRC, 1, n) > 0;
	return built;
}
#define N_BUILDERS 10

int main(void)
{
	/* ---- CONTROL: a legal even width builds everywhere and is a box ---- */
	if (builders_that_build(16) != N_BUILDERS) {
		printf("NOT A RESULT: test_box_width — a legal 16-wide box does not build through "
		       "every builder, so a refusal below would prove nothing\n");
		return 2;
	}
	size_t l16 = reac_ctrl_build_upstream_filler(frame, MASTER, SRC, 1, 16, NULL, 0);
	CHK(l16 == 628 && reac_upstream_channels(l16) == 16);
	CHK(!reac_frame_is_master_downstream(l16));
	for (int n = REAC_BOX_MIN_CHANNELS; n <= REAC_BOX_MAX_CHANNELS; n += 2) {
		size_t l = reac_ctrl_build_upstream_filler(frame, MASTER, SRC, 1, n, NULL, 0);
		CHK(l > 0 && reac_upstream_channels(l) == n && !reac_frame_is_master_downstream(l));
	}

	/* ---- VERDICT: 40 is the desk's frame ---- */
	CHK(REAC_BOX_MAX_CHANNELS == 38 && !reac_box_width_ok(REAC_MAX_CHANNELS));
	CHK(builders_that_build(40) == 0);
	CHK(reac_upstream_channels((size_t)REAC_FRAME_BYTES) < 0);

	/* ...and no odd width is a box width. */
	for (int n = 1; n <= REAC_MAX_CHANNELS + 1; n += 2) {
		CHK(!reac_box_width_ok(n));
		CHK(builders_that_build(n) == 0);
	}
	CHK(builders_that_build(0) == 0 && builders_that_build(42) == 0);

	/* The model door: a row declaring 40 either way is refused, and the widest
	 * legal row the declaration can state (36, in 4-channel slots) builds. */
	struct reac_box_model wide = *reac_box_model_by_token("fr3600");
	CHK(reac_box_model_upstream_width(&wide) == 36);
	CHK(reac_ctrl_build_as(frame, &wide, REAC_BOX_BLOCK_CONFIG, MASTER, SRC, 1, NULL, 0)
	    == reac_ctrl_box_frame_len(36));
	wide.in_ch = REAC_MAX_CHANNELS;
	CHK(reac_box_model_upstream_width(&wide) == 0);
	CHK(reac_ctrl_build_as(frame, &wide, REAC_BOX_BLOCK_CONFIG, MASTER, SRC, 1, NULL, 0) == 0);
	CHK(reac_ctrl_build_as(frame, &wide, REAC_BOX_BLOCK_CC0014, MASTER, SRC, 1, NULL, 0) == 0);
	wide.in_ch = 0; wide.out_ch = REAC_MAX_CHANNELS;   /* outputs count too */
	CHK(reac_box_model_upstream_width(&wide) == 0);
	CHK(reac_ctrl_build_as(frame, &wide, REAC_BOX_BLOCK_CONFIG, MASTER, SRC, 1, NULL, 0) == 0);
	wide.out_ch = 7;
	CHK(reac_box_model_upstream_width(&wide) == 0);

	/* The registry places a box's audio: a box width, never the desk's. */
	struct reac_boxreg r;
	reac_boxreg_init(&r, 0);
	static const uint8_t BOX[6] = { 0x00, 0x40, 0xab, 0xaa, 0xbb, 0xcc };
	CHK(reac_boxreg_add(&r, BOX, 40) < 0 && reac_boxreg_add(&r, BOX, 15) < 0);
	CHK(reac_boxreg_declare(&r, 40, "desk-wide", -1) < 0);
	CHK(reac_boxreg_add(&r, BOX, 38) == 0);   /* control: the widest box width fits */

	if (fails) {
		fprintf(stderr, "%d box-width check(s) failed\n", fails);
		return 1;
	}
	printf("OK: test_box_width — every box builder, the model door, the registry and the "
	       "upstream parser refuse 40 (the desk's frame) and every odd width; each even "
	       "2..38 builds and reads back as a box\n");
	return 0;
}
