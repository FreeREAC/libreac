// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* A CORRUPT IDENTITY REPLY IS NOT EVIDENCE. Guard for libreac review 2026-09-25, M3
 * (docs/audits/2026-09-25-libreac-review.md); red on ee205b6, green since the fix.
 *
 * reac_ctrl_identity_reply() returns 1 for a DT1 identity reply whose OUTER
 * block checksum (sum-to-0 over [18:50]) and INNER record checksum
 * (sum-to-0x80 over TAG..CKSUM) are both wrong. reac_pacer feeds its answer
 * straight into reac_identity_ingest() BEFORE reac_ctrl_classify_box_frame()
 * (which is the only door that checks the block checksum), so one corrupted
 * reply rewrites the published box firmware/REAC version. reac_ctrl.c's own
 * law: "A cdea/cfea control frame with an invalid checksum is corrupt — never
 * ... evidence of anything."
 *
 * The control arm is a real reply: the S-1608's captured firmware record (the
 * box matrix's cc0016 block, fw 2.200), whose two checksums hold. The existing
 * tests/test_identity.c fixture writes a stand-in 0x7f record checksum and no
 * block checksum at all, which is why nothing there could see this. */
#include <reac/reac_ctrlblk.h>
#include <reac/reac_identity.h>
#include <reac/reac.h>

#include <stdio.h>
#include <string.h>

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* S-1608 firmware reply, addr 0x0000, payload 02 02 00 00 (fw 2.200). */
static const uint8_t S1608_FW_BLOCK[32] = {
	0x04, 0x03, 0x00, 0x16, 0x00, 0x02, 0x00, 0xfe,
	0x11, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
	0x05, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00,
	0x77, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc,
};

static void build(uint8_t *frame)
{
	static const uint8_t OURS[6] = { 0x00, 0x40, 0xab, 0x11, 0x22, 0x33 };
	static const uint8_t BOX[6]  = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0xf6 };
	memset(frame, 0, REAC_FRAME_BYTES);
	memcpy(frame, OURS, 6);
	memcpy(frame + 6, BOX, 6);
	frame[12] = 0x88; frame[13] = 0x19;
	frame[16] = 0xcd; frame[17] = 0xea;
	memcpy(frame + REAC_CTRL_BLOCK_OFF, S1608_FW_BLOCK, 32);
	frame[REAC_FRAME_BYTES - 2] = REAC_END_MARKER_0;
	frame[REAC_FRAME_BYTES - 1] = REAC_END_MARKER_1;
}

int main(void)
{
	uint8_t frame[REAC_FRAME_BYTES];
	uint16_t addr;
	const uint8_t *pl;
	size_t pll;

	/* CONTROL: the captured reply is well-formed and is extracted. */
	build(frame);
	if (reac_ctrl_checksum_verify(frame) != 0 ||
	    reac_ctrl_record_cksum_verify(frame + REAC_CTRL_BLOCK_OFF + 16, 9) != 0 ||
	    reac_ctrl_identity_reply(frame, sizeof frame, &addr, &pl, &pll) != 1 ||
	    addr != REAC_IDENTITY_ADDR_FIRMWARE || pll != 4) {
		printf("NOT A RESULT: test_identity_cksum — the captured control reply "
		       "does not verify/extract, so the corrupt arm proves nothing\n");
		return 2;
	}

	/* VERDICT: one payload byte flipped in transit (2.200 -> 2.500). Both
	 * checksums now fail; the reply must not be extracted. */
	build(frame);
	frame[REAC_CTRL_BLOCK_OFF + 21] = 0x05;
	CHK(reac_ctrl_checksum_verify(frame) != 0);                          /* premise */
	CHK(reac_ctrl_record_cksum_verify(frame + REAC_CTRL_BLOCK_OFF + 16, 9) != 0);
	CHK(reac_ctrl_identity_reply(frame, sizeof frame, &addr, &pl, &pll) == 0);

	if (fails) {
		struct reac_identity id;
		reac_identity_init(&id);
		if (reac_ctrl_identity_reply(frame, sizeof frame, &addr, &pl, &pll) == 1)
			reac_identity_ingest(&id, addr, pl, pll);
		fprintf(stderr, "  a reply failing both checksums was ingested as fw_milli=%d "
		        "(the box sent 2200)\n", id.fw_milli);
		return 1;
	}
	printf("OK: test_identity_cksum — a reply that fails its checksums is not evidence\n");
	return 0;
}
