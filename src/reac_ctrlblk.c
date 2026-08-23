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
