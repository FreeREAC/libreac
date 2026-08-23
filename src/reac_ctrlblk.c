// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The 32-byte control block and the scene transfer built on it. See
 * reac/reac_ctrlblk.h for the protocol, the licence reason and the
 * kernel-portability commitments this file is bound by. */

#include <reac/reac_ctrlblk.h>
#include <reac/reac.h>      /* REAC_SAMPLES_PER_PKT, REAC_RESOLUTION */
#include <string.h>

/* Frame offsets, shared by the parser and the builders. A REAC frame is
 * dst[6] src[6] ethertype[2] counter[2] type[2] block[32] audio... */
#define CNT_OFF    14
#define TYPE_OFF   16
#define AUDIO_OFF  50

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

enum reac_ctrl_kind reac_ctrl_parse(const uint8_t *frame, size_t len,
                                    struct reac_ctrl_parsed *out)
{
	memset(out, 0, sizeof *out);
	if (len < AUDIO_OFF || frame[12] != 0x88 || frame[13] != 0x19) {
		out->kind = REAC_CTRL_NONE;
		return out->kind;
	}
	memcpy(out->dst, frame, 6);
	memcpy(out->src, frame + 6, 6);
	out->is_broadcast = (memcmp(frame, "\xff\xff\xff\xff\xff\xff", 6) == 0);
	out->counter = (uint16_t)(frame[CNT_OFF] | (frame[CNT_OFF + 1] << 8));
	out->op0 = frame[18]; out->op1 = frame[19];
	out->op_len = (uint16_t)((frame[20] << 8) | frame[21]);
	out->sel = frame[22];
	out->sel2 = frame[23];

	const uint8_t t0 = frame[TYPE_OFF], t1 = frame[TYPE_OFF + 1];
	if (t0 == 0x00 && t1 == 0x00) {
		out->kind = REAC_CTRL_FILLER;
	} else if (t0 == 0xcf && t1 == 0xea) {
		out->kind = REAC_CTRL_MASTER_ANNOUNCE;
	} else if (t0 == 0xce && t1 == 0xea) {
		/* A splitter's announce — the split role's own frame type, unicast to
		 * the master ~1/s, block-checksummed like every announce (reac-aes67
		 * REAC-PROTOCOL.md §6/§10.1, source-derived from reacdriver). Never
		 * yet captured on our rig (§14.1: the last unmapped type), so this
		 * names the kind and nothing more — no field decoding until a real
		 * capture grounds the layout. */
		out->kind = REAC_CTRL_SPLIT_ANNOUNCE;
	} else if (t0 == 0xcd && t1 == 0xea) {
		if (out->op0 == 0x04 && out->op1 == 0x03) {
			/* op 04 03 is a RECORD CONTAINER, not one opcode: after the
			 * 12 12 marker at [32] comes a 2-byte TAG. TAG 01 00 = the
			 * connect-grant; TAG 01 01 = a HEAD-AMP record (CH PARAM
			 * VALUE) — a live M-200 emits ~628 head-amp records per 14
			 * grants, so a joining slave must NOT read a preamp
			 * knob-turn as its grant. Every other tag (03 02, 05 00,
			 * 00 00 — the cold-connect inventory variants) stays GRANT
			 * as before (ground truth: m200-headamp-re/DECODE.md). */
			if (frame[32] == 0x12 && frame[33] == 0x12 &&
			    frame[34] == 0x01 && frame[35] == 0x01) {
				out->kind = REAC_CTRL_HEADAMP;
				out->ch    = frame[36];
				out->param = frame[37];
				out->value = frame[38];
			} else {
				out->kind = REAC_CTRL_GRANT;
			}
		} else if (out->op0 == 0x01 && out->op1 == 0x03 && out->op_len == 0x0019)
			out->kind = REAC_CTRL_MASTER_HB;       /* master established heartbeat */
		else if (out->op0 == 0x01 && out->op1 == 0x03 && out->op_len == 0x0001)
			out->kind = REAC_CTRL_BOX_HB;          /* a box keep-alive */
		else if (out->op0 == 0x01)
			out->kind = REAC_CTRL_PROBE;           /* master hunting (sub-states) */
		else
			out->kind = REAC_CTRL_UNKNOWN_CTRL;
	} else {
		out->kind = REAC_CTRL_UNKNOWN_CTRL;
	}
	return out->kind;
}

/* box-width frame length for n_ch inputs */
size_t reac_ctrl_box_frame_len(int n_ch)
{
	return (size_t)AUDIO_OFF + (size_t)n_ch * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION + 2;
}

/* The braided audio region of every box->master frame that carries audio — the
 * upstream FILLER, the broadcast presence-flood AND the cold-connect, because on
 * a real box that region varies every frame (it is live input, NOT static
 * inventory). Slot placement is plain ascending; a real M-5000 expects exactly
 * this from a box's return.
 *
 * The loop itself moved to libreac on 2026-07-29 as reac_braid_encode()
 * (<reac/reac_encode.h>): it was byte-for-byte the same loop as the downstream
 * encoder's, which is the point — the braid is the REAC wire format in BOTH
 * directions (task #108, the ex-"FPGA scramble" of task #61), and it is the
 * exact inverse of reac_upstream_decode(). What stayed here is everything the
 * layout is not: the 32-byte control block, its two nested checksums, the
 * box-model matrix and the frame envelope, all of which are handshake state
 * owned by the role FSM. Passing n_ch as both the frame width and the plane
 * count preserves the pre-move behaviour exactly (this builder is always given
 * one plane per box input).
 */

/* ---- FIXED box-model matrix (byte-verified real announce blocks) ----
 * Role decides authority (docs/REAC-BOX-STATE-DIAGRAM.md): as a SLAVE (we ARE a
 * stagebox) this matrix is LAW — we pick a row and emit its announce verbatim. As
 * a MASTER (we ARE a mixer) the box's announce on the wire is the truth and this
 * matrix is only a default. Each row is a real box's captured config-announce
 * (selector byte = displayed model family; sum mod 256 == 0 with its trailing
 * check byte), plus, for the 0x84 family, the ASCII name frame that names the
 * exact model. All blocks byte-matched to matrix-m200/m5000-s1608 / -s0808. */
static const struct reac_box_model BOX_MODELS[] = {
	{ .token = "s1608", .display = "S-1608 (16 in / 8 out)", .in_ch = 16, .out_ch = 8,
	  .config_block = {
		0x01, 0x03, 0x00, 0x10, 0x82, 0x00, 0x00, 0x02,
		0x02, 0x02, 0x02, 0x02, 0x01, 0x01, 0x03, 0x03,
		0x03, 0x03, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c },
	  .has_name = 0,     /* 0x82 family: named by selector, no ASCII frame */
	  .cc0014 = {
		0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe,
		0x0f, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x01, 0x00, 0x06, 0x00, 0x01, 0x00, 0x78, 0xf7,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	  .cc0013 = {
		0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe,
		0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x03, 0x02, 0x00, 0x01, 0x00, 0x7a, 0xf7, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 },
	  .cc0016 = {
		0x04, 0x03, 0x00, 0x16, 0x00, 0x02, 0x00, 0xfe,
		0x11, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00,
		0x77, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc },
	  .cc001a = {
		0x04, 0x03, 0x00, 0x1a, 0x00, 0x02, 0x00, 0xfe,
		0x15, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x02,
		0x00, 0x03, 0x00, 0x02, 0x6e, 0xf7, 0x00, 0xf4 },
	  .has_extra = 0 },  /* S-1608 sends no 0402000d */
	{ .token = "s0808", .display = "S-0808 (8 in / 8 out)", .in_ch = 8, .out_ch = 8,
	  .config_block = {
		0x01, 0x03, 0x00, 0x10, 0x84, 0x00, 0x00, 0x00,
		0x02, 0x02, 0x01, 0x01, 0x03, 0x03, 0x03, 0x03,
		0x03, 0x03, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a },
	  .has_name = 1,     /* 0x84 family: ASCII name frame gives the exact model */
	  .name_block = {
		0x04, 0x01, 0x00, 0x1b, 0x00, 0x02, 0x00, 0xfe,
		0x16, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x10, 0x00, 0x01, 0x53, 0x2d, 0x30,   /* "S-0" */
		0x38, 0x30, 0x38, 0x00, 0x00, 0x00, 0x00, 0x05 },  /* "808" */
	  .cc0014 = {         /* 0014/0013 match the S-1608's (model-generic so far) */
		0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe,
		0x0f, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x01, 0x00, 0x06, 0x00, 0x01, 0x00, 0x78, 0xf7,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	  .cc0013 = {
		0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe,
		0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x03, 0x02, 0x00, 0x01, 0x00, 0x7a, 0xf7, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 },
	  .cc0016 = {         /* S-0808's inventory differs from S-1608's */
		0x04, 0x03, 0x00, 0x16, 0x00, 0x02, 0x00, 0xfe,
		0x11, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x03,
		0x77, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc },
	  .cc001a = {
		0x04, 0x03, 0x00, 0x1a, 0x00, 0x02, 0x00, 0xfe,
		0x15, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01,
		0x00, 0x00, 0x00, 0x00, 0x74, 0xf7, 0x00, 0xf4 },
	  .has_extra = 1,     /* S-0808 also sends cdea 04 02 000d */
	  .extra_block = {
		0x04, 0x02, 0x00, 0x0d, 0x00, 0x02, 0x00, 0xfe,
		0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1a,
		0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd4 } },
	/* S-4000S — also 0x84 family but sends NO name frame (0x84's DEFAULT desk
	 * label IS "S-4000S") and NO 0402000d. Its config descriptor + 0016/001a
	 * inventory are distinct. Byte-verified from a real S-4000S cold boot on an
	 * M-5000 (s4000s-coldboot-m5000-2026-07-12, box c4:06:80). NOTE: captured on
	 * OHRCA (frames +2 CRC trailer); the control blocks below are generation-
	 * independent, but emulating on an OHRCA desk needs the upstream +2 (W4). */
	{ .token = "s4000s", .display = "S-4000S (32 in / 8 out)", .in_ch = 32, .out_ch = 8,
	  .config_block = {
		0x01, 0x03, 0x00, 0x10, 0x84, 0x00, 0x00, 0x00,
		0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
		0x01, 0x01, 0x03, 0x03, 0x00, 0x03, 0x00, 0x00,
		0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c },
	  .has_name = 0,     /* 0x84 DEFAULT name is "S-4000S" — no ASCII frame */
	  .cc0014 = {
		0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe,
		0x0f, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x01, 0x00, 0x06, 0x00, 0x01, 0x00, 0x78, 0xf7,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	  .cc0013 = {
		0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe,
		0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x03, 0x02, 0x00, 0x01, 0x00, 0x7a, 0xf7, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 },
	  .cc0016 = {
		0x04, 0x03, 0x00, 0x16, 0x00, 0x02, 0x00, 0xfe,
		0x11, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x00, 0x00, 0x02, 0x05, 0x00, 0x00,
		0x74, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc },
	  .cc001a = {
		0x04, 0x03, 0x00, 0x1a, 0x00, 0x02, 0x00, 0xfe,
		0x15, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x02,
		0x00, 0x01, 0x00, 0x02, 0x70, 0xf7, 0x00, 0xf4 },
	  .has_extra = 0 },  /* S-4000S sends no 0402000d */
};

const struct reac_box_model *reac_box_model_table(size_t *count)
{
	if (count) *count = sizeof(BOX_MODELS) / sizeof(BOX_MODELS[0]);
	return BOX_MODELS;
}

const struct reac_box_model *reac_box_model_by_token(const char *token)
{
	size_t n = sizeof(BOX_MODELS) / sizeof(BOX_MODELS[0]);
	for (size_t i = 0; i < n; i++)
		if (token && strcmp(BOX_MODELS[i].token, token) == 0)
			return &BOX_MODELS[i];
	return NULL;
}

/* Map an input width to its matrix row (each verified width is one model). Falls
 * back to S-1608 for widths not in the matrix so the pure builders never fault. */
const struct reac_box_model *reac_box_model_by_channels(int in_ch)
{
	size_t n = sizeof(BOX_MODELS) / sizeof(BOX_MODELS[0]);
	for (size_t i = 0; i < n; i++)
		if (BOX_MODELS[i].in_ch == in_ch)
			return &BOX_MODELS[i];
	return &BOX_MODELS[0];   /* default: S-1608 */
}

const struct reac_box_model *reac_ctrl_identify_box(const uint8_t *frame, size_t len)
{
	/* Recognize the connected box's MODEL from its config-announce
	 * (cdea 01 03 0010) by matching the 32-byte descriptor block against the
	 * fixed matrix. Each row's config_block is unique (selector + descriptor:
	 * S-1608 0x82; S-0808 / S-4000S both 0x84 but distinct descriptors), so an
	 * exact block match uniquely names the model. NULL = not a config-announce,
	 * or no known model -> caller falls back to the frame's own descriptor/width. */
	if (len < REAC_CTRL_BLOCK_OFF + 32)               return NULL;
	if (frame[12] != 0x88 || frame[13] != 0x19)       return NULL;   /* 0x8819    */
	if (frame[16] != 0xcd || frame[17] != 0xea)       return NULL;   /* cdea      */
	if (frame[18] != 0x01 || frame[19] != 0x03 ||
	    frame[20] != 0x00 || frame[21] != 0x10)       return NULL;   /* 01 03 0010 */
	size_t n; const struct reac_box_model *t = reac_box_model_table(&n);
	for (size_t i = 0; i < n; i++)
		if (memcmp(frame + REAC_CTRL_BLOCK_OFF, t[i].config_block, 32) == 0)
			return &t[i];
	return NULL;
}
