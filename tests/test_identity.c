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

	printf("test_identity: all checks passed\n");
	return 0;
}
