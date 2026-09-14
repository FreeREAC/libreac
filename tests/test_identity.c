// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_identity — the identity-page decode, pinned against the exact payload
 * bytes reac-protocol/spec/reac.ksy documents from the M-200 cold-boot corpus
 * (identity_data; worked exchange m200-BIDIR-coldboot-2026-07-11 frames
 * 3475..3479). The firmware digits are cross-checked against Roland's own
 * release packages (s0808_sys_v1003, s1608_sys_ver2200, s4000_sys_ver2500), so
 * these are not self-referential goldens. The addr 0x0600 REAC version is pinned
 * the same way: the strings this decode produces are the strings an M-200i prints
 * beside those firmwares — "REAC 2.302" for the S-1608 and "REAC 2.102" for the
 * S-4000S-3208, read off the console's display on 2026-09-14. Every negative arm proves a malformed
 * or unanswered address stays a FACT (has_* clear), never a guess. */
#include <reac/reac_identity.h>
#include <reac/reac_link_state.h>
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

/* addr 0x0600 REAC version: four u16be, a reserved word then major.minor.patch.
 * The M-200i prints the last three as `major.minorPP` — "2.302" for the S-1608 and
 * "2.102" for the S-4000S-3208, both read off the console's display 2026-09-14. */
static const uint8_t VER_S0808[8]  = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t VER_S1608[8]  = { 0x00, 0x00, 0x00, 0x02, 0x00, 0x03, 0x00, 0x02 };
static const uint8_t VER_S4000S[8] = { 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x02 };


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

/* A recording stand-in for pw_properties: reac_box_identity_publish COMPOSES AND
 * STAMPS in one act, so what a consumer reads — which key, what value, written at
 * all — is what this asserts. Merge semantics, like the real dict. */
#define FAKE_MAX 8
struct fake_props {
	char key[FAKE_MAX][40];
	char val[FAKE_MAX][40];
	int  n;
};

static void fake_set(void *ctx, const char *key, const char *value)
{
	struct fake_props *f = ctx;
	for (int i = 0; i < f->n; i++)
		if (strcmp(f->key[i], key) == 0) {
			snprintf(f->val[i], sizeof f->val[i], "%s", value);
			return;
		}
	if (f->n == FAKE_MAX)
		return;
	snprintf(f->key[f->n], sizeof f->key[f->n], "%s", key);
	snprintf(f->val[f->n], sizeof f->val[f->n], "%s", value);
	f->n++;
}

static const char *fake_get(const struct fake_props *f, const char *key)
{
	for (int i = 0; i < f->n; i++)
		if (strcmp(f->key[i], key) == 0)
			return f->val[i];
	return NULL;
}

int main(void)
{
	char buf[REAC_IDENTITY_FW_STR_CAP];
	char vbuf[REAC_IDENTITY_REAC_VER_STR_CAP];

	/* ---- S-0808: answers all three addresses ---- */
	struct reac_identity id;
	reac_identity_init(&id);
	CHK(id.has_fw == 0 && id.has_model_name == 0 && id.has_reac_version == 0);

	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_FIRMWARE, FW_S0808, 4) == 1);
	CHK(id.has_fw == 1 && id.fw_milli == 1003);
	CHK(reac_identity_fw_str(id.fw_milli, buf, sizeof buf) == 5);
	CHK(strcmp(buf, "1.003") == 0);

	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_MODEL_NAME, NAME_S0808, sizeof NAME_S0808) == 1);
	CHK(id.has_model_name == 1 && strcmp(id.model_name, "S-0808") == 0);

	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_REAC_VERSION, VER_S0808, 8) == 1);
	CHK(id.has_reac_version == 1 && memcmp(id.reac_version_raw, VER_S0808, 8) == 0);
	CHK(id.reac_version_major == 1 && id.reac_version_minor == 0 && id.reac_version_patch == 0);
	/* PREDICTED, not read: no M-200i display has been seen for an S-0808. */
	CHK(reac_identity_reac_ver_str(id.reac_version_major, id.reac_version_minor,
	                               id.reac_version_patch, vbuf, sizeof vbuf) == 5);
	CHK(strcmp(vbuf, "1.000") == 0);

	/* ---- S-1608 and S-4000S: firmware + hw block, but NO model name ---- */
	reac_identity_init(&id);
	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_FIRMWARE, FW_S1608, 4) == 1);
	CHK(id.fw_milli == 2200);
	CHK(reac_identity_fw_str(id.fw_milli, buf, sizeof buf) == 5 && strcmp(buf, "2.200") == 0);
	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_REAC_VERSION, VER_S1608, 8) == 1);
	CHK(memcmp(id.reac_version_raw, VER_S1608, 8) == 0);
	CHK(id.reac_version_major == 2 && id.reac_version_minor == 3 && id.reac_version_patch == 2);
	/* The console's own display: "REAC 2.302" beside "Firmware 2.200". */
	CHK(reac_identity_reac_ver_str(id.reac_version_major, id.reac_version_minor,
	                               id.reac_version_patch, vbuf, sizeof vbuf) == 5);
	CHK(strcmp(vbuf, "2.302") == 0);
	CHK(id.has_model_name == 0);   /* the S-1608 never answers 0x1000 — a fact */

	reac_identity_init(&id);
	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_FIRMWARE, FW_S4000S, 4) == 1);
	CHK(id.fw_milli == 2500);
	CHK(reac_identity_fw_str(id.fw_milli, buf, sizeof buf) == 5 && strcmp(buf, "2.500") == 0);
	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_REAC_VERSION, VER_S4000S, 8) == 1);
	CHK(memcmp(id.reac_version_raw, VER_S4000S, 8) == 0);
	CHK(id.reac_version_major == 2 && id.reac_version_minor == 1 && id.reac_version_patch == 2);
	/* The console's own display: "REAC 2.102" beside "Firmware 2.500". */
	CHK(reac_identity_reac_ver_str(id.reac_version_major, id.reac_version_minor,
	                               id.reac_version_patch, vbuf, sizeof vbuf) == 5);
	CHK(strcmp(vbuf, "2.102") == 0);
	/* The REAC version and the firmware are DIFFERENT numbers off DIFFERENT
	 * addresses: 2.102 against 2.500 on the same box, so neither can stand in
	 * for the other. */
	CHK(id.fw_milli == 2500);
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
	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_REAC_VERSION, VER_S0808, 7) == 0);
	CHK(id.has_reac_version == 0);

	/* Bad arguments. */
	CHK(reac_identity_ingest(NULL, REAC_IDENTITY_ADDR_FIRMWARE, FW_S0808, 4) == -1);
	CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_FIRMWARE, NULL, 4) == -1);
	CHK(reac_identity_fw_str(1003, buf, 4) == -1);   /* buffer too small */
	CHK(reac_identity_reac_ver_str(2, 3, 2, vbuf, 4) == -1);   /* buffer too small */
	CHK(reac_identity_reac_ver_str(2, 3, 2, NULL, sizeof vbuf) == -1);

	/* The pad is on the PATCH, not the minor: (2,10,2) prints 2.1002, and a
	 * two-digit patch fills the pad instead of widening. */
	CHK(reac_identity_reac_ver_str(2, 10, 2, vbuf, sizeof vbuf) == 6);
	CHK(strcmp(vbuf, "2.1002") == 0);
	CHK(reac_identity_reac_ver_str(1, 0, 25, vbuf, sizeof vbuf) == 5);
	CHK(strcmp(vbuf, "1.025") == 0);

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
		build_identity_reply(frame, REAC_IDENTITY_ADDR_REAC_VERSION, VER_S1608, 8);
		CHK(reac_ctrl_identity_reply(frame, sizeof frame, &got_addr, &pl, &pll) == 1);
		CHK(got_addr == REAC_IDENTITY_ADDR_REAC_VERSION && pll == 8);
		CHK(reac_identity_ingest(&id, got_addr, pl, pll) == 1);
		CHK(memcmp(id.reac_version_raw, VER_S1608, 8) == 0);

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

	/* ---- the BADGE: one decoded identity -> the three node properties ---- */
	{
		struct fake_props f;
		memset(&f, 0, sizeof f);

		/* An S-1608: firmware 2.200 at 0x0000, REAC 2.302 at 0x0600 — the two
		 * numbers the console displays side by side. They must land on DIFFERENT
		 * keys with DIFFERENT values; substituting one for the other is the whole
		 * defect this badge exists to prevent. */
		reac_identity_init(&id);
		CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_FIRMWARE, FW_S1608, 4) == 1);
		CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_REAC_VERSION, VER_S1608, 8) == 1);
		reac_box_identity_publish(&id, fake_set, &f);
		CHK(f.n == 3);
		CHK(strcmp(fake_get(&f, REAC_PROP_BOX_FIRMWARE), "2.200") == 0);
		CHK(strcmp(fake_get(&f, REAC_PROP_BOX_REAC_VERSION), "2.302") == 0);
		CHK(strcmp(fake_get(&f, REAC_PROP_BOX_HW), "00000002 00030002") == 0);

		/* An S-4000S-3208 over the same dict: every key is re-stamped, so a box
		 * swap cannot leave the previous box's version standing. */
		reac_identity_init(&id);
		CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_FIRMWARE, FW_S4000S, 4) == 1);
		CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_REAC_VERSION, VER_S4000S, 8) == 1);
		reac_box_identity_publish(&id, fake_set, &f);
		CHK(f.n == 3);
		CHK(strcmp(fake_get(&f, REAC_PROP_BOX_FIRMWARE), "2.500") == 0);
		CHK(strcmp(fake_get(&f, REAC_PROP_BOX_REAC_VERSION), "2.102") == 0);

		/* The box drops: an empty identity CLEARS all three rather than leaving
		 * the departed box's numbers behind a merging update. */
		reac_identity_init(&id);
		reac_box_identity_publish(&id, fake_set, &f);
		CHK(strcmp(fake_get(&f, REAC_PROP_BOX_FIRMWARE), "") == 0);
		CHK(strcmp(fake_get(&f, REAC_PROP_BOX_REAC_VERSION), "") == 0);
		CHK(strcmp(fake_get(&f, REAC_PROP_BOX_HW), "") == 0);

		/* NULL identity is the same fact; NULL set is a no-op, not a crash. */
		memset(&f, 0, sizeof f);
		reac_box_identity_publish(NULL, fake_set, &f);
		CHK(f.n == 3 && strcmp(fake_get(&f, REAC_PROP_BOX_REAC_VERSION), "") == 0);
		reac_box_identity_publish(&id, NULL, &f);
	}

	printf("test_identity: all checks passed\n");
	return 0;
}
