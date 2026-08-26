// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_identity — the identity-page decode, pinned against the exact payload
 * bytes reac-protocol/spec/reac.ksy documents from the M-200 cold-boot corpus
 * (identity_data; worked exchange m200-BIDIR-coldboot-2026-07-11 frames
 * 3475..3479). The firmware digits are cross-checked against Roland's own
 * release packages (s0808_sys_v1003, s1608_sys_ver2200, s4000_sys_ver2500), so
 * these are not self-referential goldens. Every negative arm proves a malformed
 * or unanswered address stays a FACT (has_* clear), never a guess. */
#include <reac/reac_identity.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac.h>
#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* addr 0x0000 firmware replies, one decimal digit per byte. */
static const uint8_t FW_S0808[4]  = { 0x01, 0x00, 0x00, 0x03 };  /* 1.003 */
static const uint8_t FW_S1608[4]  = { 0x02, 0x02, 0x00, 0x00 };  /* 2.200 */
static const uint8_t FW_S4000S[4] = { 0x02, 0x05, 0x00, 0x00 };  /* 2.500 */

/* addr 0x1000 model name: name_kind 0x01 then "S-0808" NUL-padded (the
 * reassembled record_fragment body, S-0808 only). */
static const uint8_t NAME_S0808[11] = {
	0x01, 0x53, 0x2d, 0x30, 0x38, 0x30, 0x38, 0x00, 0x00, 0x00, 0x00 };

/* addr 0x0600 hardware block, per-model constant (carried raw, not interpreted). */
static const uint8_t HW_S0808[8]  = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t HW_S1608[8]  = { 0x00, 0x00, 0x00, 0x02, 0x00, 0x03, 0x00, 0x02 };
static const uint8_t HW_S4000S[8] = { 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x02 };


/* Lay a single-record DT1 identity REPLY into a raw frame the way the wire
 * carries it: dst/src macs, 0x8819, type cd ea, then the control block whose
 * SysEx is f0 41 0a 00 00 12 12 <tag> <addr_lo> <payload> <cksum> f7. The block
 * checksum is not stamped — reac_ctrl_parse does not verify it, and the
 * extractor reads structure, not the outer checksum. Returns the frame length. */
static size_t build_identity_reply(uint8_t *frame, uint16_t addr_lo,
                                   const uint8_t *payload, size_t plen)
{
	memset(frame, 0, REAC_FRAME_BYTES);
	static const uint8_t OURS[6] = { 0x00, 0x40, 0xab, 0x11, 0x22, 0x33 };
	static const uint8_t BOX[6]  = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0xf6 };
	memcpy(frame, OURS, 6);
	memcpy(frame + 6, BOX, 6);
	frame[12] = 0x88; frame[13] = 0x19;
	frame[16] = 0xcd; frame[17] = 0xea;                 /* type: control */
	uint8_t *b = frame + REAC_CTRL_BLOCK_OFF;
	unsigned sysex_len = (unsigned)(13 + plen);         /* preamble..f7 + payload */
	b[0] = 0x04; b[1] = 0x03;                            /* link 4, seg SINGLE */
	b[2] = 0x00; b[3] = (uint8_t)(sysex_len + 5);        /* a length; unread here */
	b[4] = 0x00; b[5] = 0x02; b[6] = 0x00; b[7] = 0xfe;
	b[8] = (uint8_t)sysex_len;
	b[9]  = 0xf0; b[10] = 0x41; b[11] = 0x0a; b[12] = 0x00; b[13] = 0x00;
	b[14] = 0x12; b[15] = 0x12;                          /* DT model-lo, DT1 set */
	b[16] = 0x05; b[17] = 0x00;                          /* tag 0x0500 */
	b[18] = (uint8_t)(addr_lo >> 8); b[19] = (uint8_t)(addr_lo & 0xff);
	for (size_t i = 0; i < plen; i++)
		b[20 + i] = payload[i];
	b[20 + plen] = 0x7f;                                 /* stand-in SysEx cksum */
	b[21 + plen] = 0xf7;
	return REAC_FRAME_BYTES;
}

int main(void)
{
	char buf[REAC_IDENTITY_FW_STR_CAP];

	/* ---- S-0808: answers all three addresses ---- */
	struct reac_identity id;
	reac_identity_init(&id);
	CHK(id.has_fw == 0 && id.has_model_name == 0 && id.has_hw_block == 0);

	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_FIRMWARE, FW_S0808, 4) == 1);
	CHK(id.has_fw == 1 && id.fw_milli == 1003);
	CHK(reac_identity_fw_str(id.fw_milli, buf, sizeof buf) == 5);
	CHK(strcmp(buf, "1.003") == 0);

	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_MODEL_NAME, NAME_S0808, sizeof NAME_S0808) == 1);
	CHK(id.has_model_name == 1 && strcmp(id.model_name, "S-0808") == 0);

	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_HW_BLOCK, HW_S0808, 8) == 1);
	CHK(id.has_hw_block == 1 && memcmp(id.hw_block, HW_S0808, 8) == 0);

	/* ---- S-1608 and S-4000S: firmware + hw block, but NO model name ---- */
	reac_identity_init(&id);
	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_FIRMWARE, FW_S1608, 4) == 1);
	CHK(id.fw_milli == 2200);
	CHK(reac_identity_fw_str(id.fw_milli, buf, sizeof buf) == 5 && strcmp(buf, "2.200") == 0);
	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_HW_BLOCK, HW_S1608, 8) == 1);
	CHK(memcmp(id.hw_block, HW_S1608, 8) == 0);
	CHK(id.has_model_name == 0);   /* the S-1608 never answers 0x1000 — a fact */

	reac_identity_init(&id);
	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_FIRMWARE, FW_S4000S, 4) == 1);
	CHK(id.fw_milli == 2500);
	CHK(reac_identity_fw_str(id.fw_milli, buf, sizeof buf) == 5 && strcmp(buf, "2.500") == 0);
	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_HW_BLOCK, HW_S4000S, 8) == 1);
	CHK(memcmp(id.hw_block, HW_S4000S, 8) == 0);
	CHK(id.has_model_name == 0);

	/* ---- negatives: a malformed or unanswered address stays a fact ---- */
	reac_identity_init(&id);

	/* An address this decode does not model (0x1100, never answered in corpus). */
	CHK(reac_identity_ingest(&id, 0x1100u, NAME_S0808, sizeof NAME_S0808) == 0);
	CHK(id.has_model_name == 0);

	/* A firmware byte outside 0..9 is not a clean version — refused, not stored. */
	const uint8_t bad_fw[4] = { 0x01, 0x0a, 0x00, 0x03 };
	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_FIRMWARE, bad_fw, 4) == 0);
	CHK(id.has_fw == 0);

	/* A firmware reply shorter than 4 bytes carries no version. */
	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_FIRMWARE, FW_S0808, 3) == 0);
	CHK(id.has_fw == 0);

	/* A name payload that is only the kind byte is an empty name — not a name. */
	const uint8_t kind_only[1] = { 0x01 };
	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_MODEL_NAME, kind_only, 1) == 0);
	CHK(id.has_model_name == 0);

	/* A short hardware block is refused whole (no partial copy). */
	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_HW_BLOCK, HW_S0808, 7) == 0);
	CHK(id.has_hw_block == 0);

	/* Bad arguments. */
	CHK(reac_identity_ingest(NULL, REAC_IDENTITY_ADDR_FIRMWARE, FW_S0808, 4) == -1);
	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_FIRMWARE, NULL, 4) == -1);
	CHK(reac_identity_fw_str(1003, buf, 4) == -1);   /* buffer too small */

	/* A re-sent reply is idempotent (a poll may repeat). */
	reac_identity_init(&id);
	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_FIRMWARE, FW_S0808, 4) == 1);
	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_FIRMWARE, FW_S0808, 4) == 1);
	CHK(id.fw_milli == 1003);


	/* ---- the wire extractor: a received DT1 reply -> addr_lo + payload ---- */
	{
		uint8_t frame[REAC_FRAME_BYTES];
		uint16_t got_addr;
		const uint8_t *pl;
		size_t pll;

		/* S-0808 firmware reply: addr 0x0000, payload 01 00 00 03. */
		build_identity_reply(frame, REAC_IDENTITY_ADDR_FIRMWARE, FW_S0808, 4);
		CHK(reac_ctrl_identity_reply(frame, sizeof frame, &got_addr, &pl, &pll) == 1);
		CHK(got_addr == REAC_IDENTITY_ADDR_FIRMWARE && pll == 4);
		reac_identity_init(&id);
		CHK(reac_identity_ingest(&id, got_addr, pl, pll) == 1);
		CHK(id.fw_milli == 1003);

		/* S-1608 hardware block reply: addr 0x0600, eight bytes. */
		build_identity_reply(frame, REAC_IDENTITY_ADDR_HW_BLOCK, HW_S1608, 8);
		CHK(reac_ctrl_identity_reply(frame, sizeof frame, &got_addr, &pl, &pll) == 1);
		CHK(got_addr == REAC_IDENTITY_ADDR_HW_BLOCK && pll == 8);
		CHK(reac_identity_ingest(&id, got_addr, pl, pll) == 1);
		CHK(memcmp(id.hw_block, HW_S1608, 8) == 0);

		/* An RQ1 POLL (command 0x11), not a reply, is not extracted. */
		build_identity_reply(frame, REAC_IDENTITY_ADDR_FIRMWARE, FW_S0808, 4);
		frame[REAC_CTRL_BLOCK_OFF + 15] = REAC_DT1_CMD_RQ1;   /* 0x12 -> 0x11 */
		CHK(reac_ctrl_identity_reply(frame, sizeof frame, &got_addr, &pl, &pll) == 0);

		/* A non-identity frame (a FILLER) is not extracted. */
		memset(frame, 0, sizeof frame);
		frame[12] = 0x88; frame[13] = 0x19;   /* type 00 00 = filler */
		CHK(reac_ctrl_identity_reply(frame, sizeof frame, &got_addr, &pl, &pll) == 0);

		/* NULL arguments. */
		CHK(reac_ctrl_identity_reply(NULL, sizeof frame, &got_addr, &pl, &pll) == -1);
	}

	printf("test_identity: all checks passed\n");
	return 0;
}
