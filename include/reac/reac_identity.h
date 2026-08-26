// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_identity — the box's own answer to "what model are you and what firmware
 * do you run", decoded from the REAC IDENTITY PAGE (DT1 tag 0x0500).
 *
 * This is protocol, not policy — the same reason reac_ctrlblk lives here. The
 * grammar of the page is pinned in reac-protocol/spec/reac.ksy (`identity_data`);
 * this is its runtime decode, so every consumer reads the box identity the same
 * way rather than re-deriving the address map.
 *
 * # The page is ADDRESSED, not a fixed struct
 *
 * 0x0500 is only the HIGH half of a Roland 4-byte DT1 address; the LOW half
 * (`addr_lo`) selects which record the payload carries. One tag covers six
 * records, so a consumer reads them ONE reply at a time and accumulates. A box
 * answers only the addresses it implements — the S-1608 and S-4000S answer the
 * firmware version and the hardware block and NOTHING ELSE; only the S-0808 ever
 * returns a model name. That is a real limit of the page, so every field below
 * carries a `has_*` flag: an unanswered address is a FACT, never a zero.
 *
 * What is RESOLVED (reac.ksy documents the evidence):
 *   - addr 0x0000 FIRMWARE VERSION — four bytes, one DECIMAL DIGIT each, most
 *     significant first, written D.DDD. 01 00 00 03 -> 1.003 (S-0808's
 *     s0808_sys_v1003), 02 02 00 00 -> 2.200 (S-1608), 02 05 00 00 -> 2.500
 *     (S-4000S). Matched against Roland's own release packages, not ourselves.
 *   - addr 0x1000 MODEL NAME — one name_kind byte (0x01) then a 16-byte
 *     NUL-padded ASCII field ("S-0808"). Only the S-0808 implements it.
 *
 * What is UNRESOLVED and therefore carried as RAW BYTES, never interpreted:
 *   - addr 0x0600 HARDWARE BLOCK — eight bytes, per-model constant and identical
 *     between the two units of each model captured. The second u16be tracks the
 *     REAC port count; the rest is unexplained. The box's boot menu names four
 *     versions (Main, Boot, FPGA, REAC), so a REAC-protocol version PLAUSIBLY
 *     lives here — but that is unproven, so this decode exposes the bytes and
 *     names nothing. Do not label this "the REAC version".
 *
 * KERNEL-PORTABLE BY CONSTRUCTION, matching reac_ctrlblk: no allocation, no
 * floating point, caller-owned buffers with explicit lengths, failure as a
 * return value, stdint/stddef and memcpy/memset only.
 */
#ifndef REAC_IDENTITY_H
#define REAC_IDENTITY_H

#include <stdint.h>
#include <stddef.h>

/* DT1 tag of the identity page: the high half of the Roland 4-byte address. */
#define REAC_IDENTITY_TAG 0x0500u

/* The `addr_lo` selectors this decode understands (reac.ksy `identity_addr`). */
#define REAC_IDENTITY_ADDR_FIRMWARE 0x0000u
#define REAC_IDENTITY_ADDR_HW_BLOCK 0x0600u
#define REAC_IDENTITY_ADDR_MODEL_NAME 0x1000u

/* The 16-byte ASCII model name plus its NUL terminator. */
#define REAC_IDENTITY_NAME_CAP 17u
/* The per-model hardware block at addr 0x0600. */
#define REAC_IDENTITY_HW_LEN 8u
/* A firmware string "D.DDD" plus NUL never exceeds this. */
#define REAC_IDENTITY_FW_STR_CAP 8u

/* One box's decoded identity, accumulated across the page's per-address replies.
 * Zero-initialise with reac_identity_init before the first ingest. */
struct reac_identity {
	/* Firmware version times 1000: 1003 is 1.003. Valid only when has_fw. */
	uint16_t fw_milli;
	uint8_t  has_fw;
	/* NUL-terminated ASCII model name; empty unless has_model_name. */
	char     model_name[REAC_IDENTITY_NAME_CAP];
	uint8_t  has_model_name;
	/* The raw, uninterpreted hardware block. Valid only when has_hw_block. */
	uint8_t  hw_block[REAC_IDENTITY_HW_LEN];
	uint8_t  has_hw_block;
};

/* Clear an identity to "nothing seen yet". */
void reac_identity_init(struct reac_identity *id);

/* Ingest one DT1 REPLY record off the identity page: `addr_lo` is the low half
 * of the address, `payload`/`len` the record's contents (which may be SHORTER
 * than the console asked for). The caller has already established this is a DT1
 * reply on tag 0x0500 — this function only dispatches on `addr_lo` and stores.
 *
 * Returns 1 when a field was recognised and stored, 0 when the record was
 * ignored (an address this decode does not model, or a payload too short or
 * malformed to trust — e.g. a firmware byte outside 0..9), and <0 on a bad
 * argument (NULL id, or NULL payload with a non-zero len). Idempotent per
 * address: a re-sent reply overwrites with the same value. */
int reac_identity_ingest(struct reac_identity *id, uint16_t addr_lo,
                         const uint8_t *payload, size_t len);

/* Format a fw_milli as "D.DDD" into `out` (needs REAC_IDENTITY_FW_STR_CAP).
 * Returns the string length written, or <0 if the buffer is too small or `out`
 * is NULL. Independent of a struct so a caller can format a bare version. */
int reac_identity_fw_str(uint16_t fw_milli, char *out, size_t cap);

#endif /* REAC_IDENTITY_H */
