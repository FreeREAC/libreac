// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libFuzzer harness: the SCENE PUSH framing (reac_ctrl_build_scene_step,
 * reac_ctrl_scene_set_mac, reac_ctrl_scene_build in src/reac_ctrlblk.c) over a
 * scene body (src/reac_scene_body.c holds the one libreac ships).
 *
 * The scene body is the largest object libreac puts on the wire, and the framing
 * is the only code that indexes into it: 8904 bytes cut into a 24-byte FIRST
 * chunk, 341 MIDDLE chunks of 26 and a 14-byte LAST one. The harness hands the
 * framing a body of any length and any step index, so its two guards are what
 * stands between a caller's mistake and a read past the body.
 *
 * Input:  [0]     flags: bit 0 = the body is REAC_SCENE_BYTES long whatever [1..2]
 *                 says; bit 1 = also run the whole transfer and reassemble it
 *         [1..2]  u16 LE body length (when bit 0 is clear)
 *         [3..4]  s16 LE step index
 *         [5..10] the MAC to substitute
 *         [11..]  bytes XORed over reac_scene_placeholder from offset 0, so the
 *                 body starts as the real one and drifts from it
 *
 * The body is its own allocation of exactly its length.
 *
 * Properties, each an abort():
 *   - each door accepts exactly a REAC_SCENE_BYTES body (and the step builder
 *     exactly a step in [0, REAC_SCENE_STEPS)), and refuses everything else;
 *   - a built step is a cd ea link-1 bulk block whose 32 bytes sum to zero and
 *     whose payload is the body's bytes at that step's offset;
 *   - THE TRANSFER CARRIES THE BODY: the payload lengths every step declares sum to
 *     the total the FIRST step declares, and the payloads concatenated in step
 *     order are the body, byte for byte; reac_ctrl_parse reads every step back as
 *     SCENE_TRANSFER in the segment state it was built with;
 *   - set_mac writes the MAC at REAC_SCENE_MAC_OFF and nothing else; scene_build
 *     lays down the three tags the box validates.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <reac/reac_ctrlblk.h>
#include <reac/reac_scene_body.h>

#include "fuzz.h"

static int block_sums_to_zero(const uint8_t blk[34])
{
	unsigned s = 0;
	for (int i = 2; i < 34; i++)
		s += blk[i];
	return (s & 0xff) == 0;
}

/* Offset and length of the body bytes step `k` carries. */
static void step_slice(int k, size_t *off, size_t *n)
{
	if (k == 0) {
		*off = 0;
		*n = REAC_SCENE_HEAD_BYTES;
	} else if (k <= REAC_SCENE_CHUNKS) {
		*off = REAC_SCENE_HEAD_BYTES + (size_t)(k - 1) * REAC_SCENE_CHUNK_BYTES;
		*n = REAC_SCENE_CHUNK_BYTES;
	} else {
		*off = REAC_SCENE_BYTES - REAC_SCENE_TAIL_BYTES;
		*n = REAC_SCENE_TAIL_BYTES;
	}
}

static void check_step(const uint8_t blk[34], const uint8_t *body, int k)
{
	/* type word cd ea, link 1, opcode 0x00 (bulk), block checksum closed */
	if (blk[0] != 0xcd || blk[1] != 0xea || blk[2] != REAC_LINK_CTRL ||
	    blk[6] != REAC_OP_BULK || !block_sums_to_zero(blk))
		abort();
	uint8_t seg = k == 0 ? REAC_SEG_FIRST : k <= REAC_SCENE_CHUNKS ? REAC_SEG_MIDDLE
	                                                               : REAC_SEG_LAST;
	if (blk[3] != seg)
		abort();
	size_t off, n;
	step_slice(k, &off, &n);
	if ((size_t)(blk[4] << 8 | blk[5]) != n)
		abort();   /* the declared payload length is this step's share */
	const uint8_t *pay = blk + (k == 0 ? 9 : 7);
	if (memcmp(pay, body + off, n) != 0)
		abort();

	/* reac_ctrl_parse reads the step back, framed the way a master stamps it. */
	uint8_t frame[50] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x40, 0xab, 0, 0, 1,
	                      0x88, 0x19 };
	memcpy(frame + 16, blk, 34);
	struct reac_ctrl_parsed p;
	if (reac_ctrl_parse(frame, sizeof frame, &p) != REAC_CTRL_SCENE_TRANSFER ||
	    p.link != REAC_LINK_CTRL || p.seg != seg || p.opcode != REAC_OP_BULK ||
	    p.blk_len != n || reac_ctrl_checksum_verify(frame) != 0)
		abort();
}

static void whole_transfer(const uint8_t *body)
{
	uint8_t *again = malloc(REAC_SCENE_BYTES);
	if (!again)
		abort();
	size_t got = 0, declared = 0;
	for (int k = 0; k < REAC_SCENE_STEPS; k++) {
		uint8_t blk[34];
		if (reac_ctrl_build_scene_step(blk, body, REAC_SCENE_BYTES, k) != 0)
			abort();
		check_step(blk, body, k);
		size_t n = (size_t)(blk[4] << 8 | blk[5]);
		if (k == 0)
			declared = (size_t)(blk[7] << 8 | blk[8]);
		if (got + n > REAC_SCENE_BYTES)
			abort();
		memcpy(again + got, blk + (k == 0 ? 9 : 7), n);
		got += n;
	}
	if (declared != REAC_SCENE_BYTES || got != REAC_SCENE_BYTES ||
	    memcmp(again, body, REAC_SCENE_BYTES) != 0)
		abort();
	free(again);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size >= FUZZ_INPUT_MAX)
		return 0;
	struct fuzz_in in = fuzz_in(data, size);
	uint8_t flags = fuzz_u8(&in);
	size_t n = fuzz_u16(&in);
	int step = (int16_t)fuzz_u16(&in);
	uint8_t mac[6];
	for (int i = 0; i < 6; i++)
		mac[i] = fuzz_u8(&in);
	if (flags & 1)
		n = REAC_SCENE_BYTES;

	uint8_t *body = malloc(n ? n : 1);
	if (!body)
		abort();
	for (size_t i = 0; i < n; i++)
		body[i] = i < REAC_SCENE_BYTES ? reac_scene_placeholder[i] : 0;
	const uint8_t *x;
	size_t nx = fuzz_take(&in, in.n, &x);
	for (size_t i = 0; i < nx && i < n; i++)
		body[i] ^= x[i];

	const int whole = n == REAC_SCENE_BYTES;
	uint8_t blk[34];
	int r = reac_ctrl_build_scene_step(blk, body, n, step);
	if (r != ((whole && step >= 0 && step < REAC_SCENE_STEPS) ? 0 : -1))
		abort();
	if (r == 0)
		check_step(blk, body, step);
	if (whole && (flags & 2))
		whole_transfer(body);

	uint8_t *before = fuzz_dup(body, n);
	r = reac_ctrl_scene_set_mac(body, n, mac);
	if (r != (whole ? 0 : -1))
		abort();
	if (r == 0) {
		if (memcmp(body + REAC_SCENE_MAC_OFF, mac, 6) != 0 ||
		    memcmp(body, before, REAC_SCENE_MAC_OFF) != 0 ||
		    memcmp(body + REAC_SCENE_MAC_OFF + 6, before + REAC_SCENE_MAC_OFF + 6,
		           n - REAC_SCENE_MAC_OFF - 6) != 0)
			abort();
	} else if (memcmp(body, before, n) != 0) {
		abort();   /* a refused call writes nothing */
	}

	r = reac_ctrl_scene_build(body, n, mac);
	if (r != (whole ? 0 : -1))
		abort();
	if (r == 0 &&
	    (memcmp(body + REAC_SCENE_TAG_ID_OFF, "1234", 4) != 0 ||
	     memcmp(body + REAC_SCENE_TAG_SYSP_OFF, "SYSP", 4) != 0 ||
	     memcmp(body + REAC_SCENE_TAG_SCEN_OFF, "SCEN", 4) != 0 ||
	     memcmp(body + REAC_SCENE_MAC_OFF, mac, 6) != 0))
		abort();
	free(before);
	free(body);
	return 0;
}
