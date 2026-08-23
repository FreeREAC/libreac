// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The 32-byte control block and the scene transfer built on it. See
 * reac/reac_ctrlblk.h for the protocol, the licence reason and the
 * kernel-portability commitments this file is bound by. */

#include <reac/reac_ctrlblk.h>
#include <string.h>

void reac_ctrl_block_cksum_stamp(uint8_t block[REAC_CTRL_BLOCK_LEN])
{
	unsigned s = 0;
	for (int i = 0; i < REAC_CTRL_BLOCK_LEN - 1; i++)
		s += block[i];
	block[REAC_CTRL_BLOCK_LEN - 1] = (uint8_t)((256 - (s & 0xff)) & 0xff);
}

void reac_ctrl_checksum_apply(uint8_t *frame)
{
	reac_ctrl_block_cksum_stamp(frame + REAC_CTRL_BLOCK_OFF);
}

int reac_ctrl_checksum_verify(const uint8_t *frame)
{
	unsigned s = 0;
	for (int i = REAC_CTRL_BLOCK_OFF; i < REAC_CTRL_BLOCK_END; i++)
		s += frame[i];
	return (s & 0xff) == 0 ? 0 : -1;
}

/* The nested record checksum sums to 0x80, not to 0. */
void reac_ctrl_record_cksum_stamp(uint8_t *rec, size_t n)
{
	unsigned s = 0;
	for (size_t i = 0; i + 1 < n; i++)
		s += rec[i];
	rec[n - 1] = (uint8_t)((0x80 - s) & 0xff);
}

int reac_ctrl_record_cksum_verify(const uint8_t *rec, size_t n)
{
	unsigned s = 0;
	for (size_t i = 0; i < n; i++)
		s += rec[i];
	return ((s & 0xff) == 0x80) ? 0 : -1;
}

/* ---- the scene push ------------------------------------------------------
 * Every step is the same 34-byte [type|block] shape: cd ea, the 2-byte op, the
 * BE payload length, one reserved 0x00, then the payload, checksum last.
 *
 * The length a step declares is its PAYLOAD length, and those lengths are
 * exactly what sum to the declared total — so the three sizes and the total are
 * one fact, not four. A body whose lengths do not sum to what the header
 * declares leaves the box waiting for bytes that never come. */
#define SCENE_OP_OFF          2   /* [2:4]  the 2-byte op                */
#define SCENE_LEN_OFF         4   /* [4:6]  BE payload length            */
#define SCENE_PAY_OFF         7   /* [7:..] payload ([6] stays reserved) */
#define SCENE_HEAD_TOTAL_OFF  7   /* header only: BE total, payload at [9] */

int reac_ctrl_build_scene_step(uint8_t blk[34], const uint8_t *body, size_t n,
                               int step)
{
	if (!blk || !body || n != REAC_SCENE_BYTES)
		return -1;
	if (step < 0 || step >= REAC_SCENE_STEPS)
		return -1;

	memset(blk, 0, 34);
	blk[0] = 0xcd; blk[1] = 0xea;

	if (step == 0) {
		blk[SCENE_OP_OFF] = 0x01; blk[SCENE_OP_OFF + 1] = 0x01;
		blk[SCENE_LEN_OFF]     = (uint8_t)(REAC_SCENE_HEAD_BYTES >> 8);
		blk[SCENE_LEN_OFF + 1] = (uint8_t)(REAC_SCENE_HEAD_BYTES & 0xff);
		blk[SCENE_HEAD_TOTAL_OFF]     = (uint8_t)(REAC_SCENE_BYTES >> 8);
		blk[SCENE_HEAD_TOTAL_OFF + 1] = (uint8_t)(REAC_SCENE_BYTES & 0xff);
		memcpy(blk + SCENE_HEAD_TOTAL_OFF + 2, body, REAC_SCENE_HEAD_BYTES);
	} else if (step <= REAC_SCENE_CHUNKS) {
		size_t off = REAC_SCENE_HEAD_BYTES +
		             (size_t)(step - 1) * REAC_SCENE_CHUNK_BYTES;
		blk[SCENE_OP_OFF] = 0x01; blk[SCENE_OP_OFF + 1] = 0x00;
		blk[SCENE_LEN_OFF]     = (uint8_t)(REAC_SCENE_CHUNK_BYTES >> 8);
		blk[SCENE_LEN_OFF + 1] = (uint8_t)(REAC_SCENE_CHUNK_BYTES & 0xff);
		memcpy(blk + SCENE_PAY_OFF, body + off, REAC_SCENE_CHUNK_BYTES);
	} else {
		/* The final chunk fills the SAME 26-byte payload slot as every other one
		 * but declares only REAC_SCENE_TAIL_BYTES of it as body — the transfer
		 * ends mid-slot. The 12 bytes behind the body are a fixed trailer, not
		 * desk state: identical in every op-0102 of both the M-200i and the M-300
		 * establish captures (3/3 each), so they are reproduced rather than
		 * zeroed. Zeroing still satisfies the declared length, but this box has
		 * punished "functionally equivalent" before. */
		static const uint8_t TAIL_TRAILER[12] = {
			0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
			0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
		};
		size_t off = REAC_SCENE_BYTES - REAC_SCENE_TAIL_BYTES;
		blk[SCENE_OP_OFF] = 0x01; blk[SCENE_OP_OFF + 1] = 0x02;
		blk[SCENE_LEN_OFF]     = (uint8_t)(REAC_SCENE_TAIL_BYTES >> 8);
		blk[SCENE_LEN_OFF + 1] = (uint8_t)(REAC_SCENE_TAIL_BYTES & 0xff);
		memcpy(blk + SCENE_PAY_OFF, body + off, REAC_SCENE_TAIL_BYTES);
		memcpy(blk + SCENE_PAY_OFF + REAC_SCENE_TAIL_BYTES,
		       TAIL_TRAILER, sizeof TAIL_TRAILER);
	}

	reac_ctrl_block_cksum_stamp(blk + 2);
	return 0;
}

int reac_ctrl_scene_set_mac(uint8_t *body, size_t n, const uint8_t mac[6])
{
	if (!body || !mac || n != REAC_SCENE_BYTES)
		return -1;
	memcpy(body + REAC_SCENE_MAC_OFF, mac, 6);
	return 0;
}

int reac_ctrl_scene_build(uint8_t *body, size_t n, const uint8_t mac[6])
{
	if (!body || !mac || n != REAC_SCENE_BYTES)
		return -1;

	memset(body, 0, REAC_SCENE_BYTES);
	memcpy(body + REAC_SCENE_TAG_ID_OFF,   "1234", 4);
	memcpy(body + REAC_SCENE_TAG_SYSP_OFF, "SYSP", 4);
	memcpy(body + REAC_SCENE_TAG_SCEN_OFF, "SCEN", 4);
	memcpy(body + REAC_SCENE_MAC_OFF,      mac,    6);
	return 0;
}

/* ---- head-amp sensitivity ------------------------------------------------
 * Cumulative GAIN for each of the 56 SENS steps, in hundredths of a dB, built
 * from the firmware table's stage structure (breaks at 8, 24, 40) with the
 * per-stage step measured on the metal. See reac/reac_ctrlblk.h for how each
 * number was obtained and which one is a guess.
 *
 * Integer hundredths, not floats: this file is written to stay kernel-portable,
 * and a gain curve is exactly the place a float would sneak in. */
static const short SENS_GAIN_CDB[REAC_HEADAMP_SENS_MAX + 1] = {
	    0,    90,   180,   270,   360,   450,   540,   630,
	  630,   720,   810,   900,   990,  1080,  1170,  1260,
	 1350,  1440,  1530,  1620,  1710,  1800,  1890,  1980,
	 1980,  2075,  2170,  2265,  2360,  2455,  2550,  2645,
	 2740,  2835,  2930,  3025,  3120,  3215,  3310,  3405,
	 3405,  3503,  3601,  3699,  3797,  3895,  3993,  4091,
	 4189,  4287,  4385,  4483,  4581,  4679,  4777,  4875,
};

/* Step 0 is the least gain, and its sensitivity is the reference the desk
 * publishes: -10 dBu reaches nominal with no pad. More gain means a smaller
 * signal suffices, so sensitivity falls as the step rises. */
#define SENS_REF_CDB   (-1000)   /* step 0, pad off */
#define SENS_PAD_CDB    (2000)   /* the pad's 20 dB */

int reac_headamp_sens_cdb(uint8_t value, int pad_on)
{
	if (value > REAC_HEADAMP_SENS_MAX)
		value = REAC_HEADAMP_SENS_MAX;
	return SENS_REF_CDB - SENS_GAIN_CDB[value] + (pad_on ? SENS_PAD_CDB : 0);
}

uint8_t reac_headamp_sens_value_cdb(int cdb, int pad_on)
{
	/* Nearest step. The curve is monotonic but NOT uniform, so this is a search
	 * for the closest entry rather than arithmetic on a step size — which is the
	 * whole reason the table exists. */
	int want = SENS_REF_CDB + (pad_on ? SENS_PAD_CDB : 0) - cdb;   /* gain wanted */
	int best = 0, best_err = -1;
	for (int i = 0; i <= REAC_HEADAMP_SENS_MAX; i++) {
		int err = SENS_GAIN_CDB[i] - want;
		if (err < 0)
			err = -err;
		/* On a TIE, take the HIGHER step. The three stage breaks put two steps
		 * on the same gain (7/8, 23/24, 39/40) — the coarse stage changes while
		 * the fine code resets, so gain is unchanged. They are not equivalent
		 * though: the upper twin sits in the quieter stage, measured 6.06 dB
		 * lower noise floor at 24 than at 23 for the same gain. Same gain, less
		 * hiss, so it is strictly the better choice. */
		if (best_err < 0 || err <= best_err) {
			best_err = err;
			best = i;
		}
	}
	return (uint8_t)best;
}

/* Whole-dB wrappers. Lossy: see the header. Rounds to nearest, away from zero. */
int reac_headamp_sens_db(uint8_t value, int pad_on)
{
	int c = reac_headamp_sens_cdb(value, pad_on);
	return (c >= 0) ? (c + 50) / 100 : -((-c + 50) / 100);
}

uint8_t reac_headamp_sens_value(int db, int pad_on)
{
	return reac_headamp_sens_value_cdb(db * 100, pad_on);
}
