// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* A BOX'S WIDTH IS EVEN PER DIRECTION, 2..40. Operator ruling 2026-09-25:
 * "BOX_MAX_CHANNELS = 40. We are dealing with a S-4000S-3208 (32 in, 8 out), we
 * also have S-2416 (24 in, 16 out), and we tested an 8 in / 32 out box."
 * (reac_box_width_ok in reac.h; reac-protocol protocol-facts.yaml `box_width`.)
 * A 40-wide box is legal, and nothing here reads it as a desk — that is
 * tests/test_desk_or_box.c's job, by direction and role.
 *
 *   CONTROL  every even width 2..40 — 40 included — builds through every box
 *            builder and reads back as that width;
 *   VERDICT  every box builder, the model door, the registry and the upstream
 *            parser refuse every odd width 1..41 and anything above 40. */
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
	/* ---- CONTROL: every even width 2..40 builds everywhere, 40 included ---- */
	if (builders_that_build(16) != N_BUILDERS || builders_that_build(40) != N_BUILDERS) {
		printf("NOT A RESULT: test_box_width — a legal box width (16 or 40) does not build "
		       "through every builder, so a refusal below would prove nothing\n");
		return 2;
	}
	CHK(REAC_BOX_MIN_CHANNELS == 2 && REAC_BOX_MAX_CHANNELS == REAC_MAX_CHANNELS);
	for (int n = REAC_BOX_MIN_CHANNELS; n <= REAC_BOX_MAX_CHANNELS; n += 2) {
		CHK(reac_box_width_ok(n));
		size_t l = reac_ctrl_build_upstream_filler(frame, MASTER, SRC, 1, n, NULL, 0);
		CHK(l == reac_ctrl_box_frame_len(n) && reac_upstream_channels(l) == n);
	}
	CHK(reac_upstream_channels((size_t)REAC_FRAME_BYTES) == REAC_MAX_CHANNELS);

	/* ---- VERDICT: no odd width, nothing past the fabric ---- */
	for (int n = 1; n <= REAC_MAX_CHANNELS + 1; n += 2) {
		CHK(!reac_box_width_ok(n));
		CHK(builders_that_build(n) == 0);
	}
	CHK(!reac_box_width_ok(0) && builders_that_build(0) == 0);
	CHK(!reac_box_width_ok(42) && builders_that_build(42) == 0);
	CHK(reac_upstream_channels(reac_ctrl_box_frame_len(42)) < 0);

	/* The model door: the 40-in and 40-out experiment rows build; a row wider than
	 * the fabric, or odd, either way, is refused. */
	const struct reac_box_model *fr40 = reac_box_model_by_token("fr4000");
	CHK(fr40 && reac_box_model_upstream_width(fr40) == 40);
	CHK(reac_ctrl_build_as(frame, fr40, REAC_BOX_BLOCK_CONFIG, MASTER, SRC, 1, NULL, 0)
	    == reac_ctrl_box_frame_len(40));
	CHK(reac_box_model_upstream_width(reac_box_model_by_token("fr0040")) == 2);
	struct reac_box_model wide = *fr40;
	wide.in_ch = 42;
	CHK(reac_box_model_upstream_width(&wide) == 0);
	CHK(reac_ctrl_build_as(frame, &wide, REAC_BOX_BLOCK_CONFIG, MASTER, SRC, 1, NULL, 0) == 0);
	CHK(reac_ctrl_build_as(frame, &wide, REAC_BOX_BLOCK_CC0014, MASTER, SRC, 1, NULL, 0) == 0);
	wide.in_ch = 0; wide.out_ch = 42;   /* outputs count too */
	CHK(reac_box_model_upstream_width(&wide) == 0);
	wide.out_ch = 7;
	CHK(reac_box_model_upstream_width(&wide) == 0);
	CHK(reac_ctrl_build_as(frame, &wide, REAC_BOX_BLOCK_CONFIG, MASTER, SRC, 1, NULL, 0) == 0);

	/* The registry places a box's audio: a 40-wide box fills the fabric. */
	struct reac_boxreg r;
	reac_boxreg_init(&r, 0);
	static const uint8_t BOX[6] = { 0x00, 0x40, 0xab, 0xaa, 0xbb, 0xcc };
	CHK(reac_boxreg_add(&r, BOX, 15) < 0 && reac_boxreg_add(&r, BOX, 42) < 0);
	CHK(reac_boxreg_add(&r, BOX, 40) == 0 && r.box[0].base == 0);

	if (fails) {
		fprintf(stderr, "%d box-width check(s) failed\n", fails);
		return 1;
	}
	printf("OK: test_box_width — each even width 2..40 (40 included) builds through every "
	       "box builder, the model door and the registry and reads back as that width; "
	       "every odd width and anything past 40 is refused\n");
	return 0;
}
