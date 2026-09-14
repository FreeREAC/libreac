// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_identity — the box's own answer to "what model are you, what firmware do
 * you run and what REAC version do you speak", decoded from the REAC IDENTITY
 * PAGE (DT1 tag 0x0500).
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
 * firmware version and the REAC version and NOTHING ELSE; only the S-0808 ever
 * returns a model name. That is a real limit of the page, so every field below
 * carries a `has_*` flag: an unanswered address is a FACT, never a zero.
 *
 * # addr 0x0000 — the FIRMWARE version
 *
 * Four bytes, one DECIMAL DIGIT each, most significant first, written D.DDD.
 * 01 00 00 03 -> 1.003 (S-0808's s0808_sys_v1003), 02 02 00 00 -> 2.200
 * (S-1608), 02 05 00 00 -> 2.500 (S-4000S). Matched against Roland's own release
 * packages, not against ourselves.
 *
 * # addr 0x0600 — the REAC version
 *
 * Eight bytes, four u16be: a reserved word then MAJOR, MINOR, PATCH. The console
 * prints it `major.minorPP` — minor and patch run together, patch zero-padded to
 * two digits — which is why 2.302 is version 2.3.2 and not "two point three
 * hundred and two".
 *
 *   S-1608        00 00 00 02 00 03 00 02  = (0, 2, 3, 2)  -> "2.302"
 *   S-4000S-3208  00 00 00 02 00 01 00 02  = (0, 2, 1, 2)  -> "2.102"
 *   S-0808        00 00 00 01 00 00 00 00  = (0, 1, 0, 0)  -> "1.000"
 *
 * EVIDENCE: the operator read an M-200i's own identity display on 2026-09-14. It
 * shows the S-1608 as "REAC 2.302" beside "Firmware 2.200", and the S-4000S-3208
 * (32 in / 8 out; the bench units 00:40:ab:c4:06:80 and 00:40:ab:c4:08:bc) as
 * "REAC 2.102" beside "Firmware 2.500" — the two versions are different numbers
 * from different addresses, and the console displays both. The console's REAC
 * string is exactly what the 0x0600 block decodes to under the rule above, for
 * two models at once. The S-0808's "1.000" is this decode's PREDICTION; no
 * display has been read for it.
 *
 * The first u16 is 0 on all three models and stays UNKNOWN — it is not decoded,
 * only carried in the raw bytes. An older reading of this block, that its second
 * u16 tracked the box's REAC port count, is a coincidence of the corpus (one port
 * on the S-0808, two on the other two models, against major 1 and 2) and is
 * dropped.
 *
 * # addr 0x1000 — the MODEL NAME
 *
 * One name_kind byte (0x01) then a 16-byte NUL-padded ASCII field ("S-0808").
 * Only the S-0808 implements it.
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
#define REAC_IDENTITY_ADDR_REAC_VERSION 0x0600u
#define REAC_IDENTITY_ADDR_MODEL_NAME 0x1000u

/* The 16-byte ASCII model name plus its NUL terminator. */
#define REAC_IDENTITY_NAME_CAP 17u
/* The REAC-version record at addr 0x0600: four u16be. */
#define REAC_IDENTITY_REAC_VER_LEN 8u
/* A firmware string "D.DDD" plus NUL never exceeds this. */
#define REAC_IDENTITY_FW_STR_CAP 8u
/* A REAC-version string "major.minorPP" plus NUL, with room for numbers wider
 * than any box has sent (five digits each) rather than a truncation. */
#define REAC_IDENTITY_REAC_VER_STR_CAP 20u

/* One box's decoded identity, accumulated across the page's per-address replies.
 * Zero-initialise with reac_identity_init before the first ingest. */
struct reac_identity {
	/* Firmware version times 1000: 1003 is 1.003. Valid only when has_fw. */
	uint16_t fw_milli;
	uint8_t  has_fw;
	/* NUL-terminated ASCII model name; empty unless has_model_name. */
	char     model_name[REAC_IDENTITY_NAME_CAP];
	uint8_t  has_model_name;
	/* The REAC protocol version the box speaks, decoded from addr 0x0600, and
	 * the record's raw bytes beside it — the raw form is kept because the first
	 * u16 is undecoded and a consumer must be able to see it. All four are valid
	 * only when has_reac_version. */
	uint16_t reac_version_major;
	uint16_t reac_version_minor;
	uint16_t reac_version_patch;
	uint8_t  reac_version_raw[REAC_IDENTITY_REAC_VER_LEN];
	uint8_t  has_reac_version;
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

/* Format a REAC version the way the console prints it — `major.minorPP`, the
 * patch zero-padded to two digits, so (2,3,2) is "2.302" and (2,1,2) is "2.102".
 * `out` needs REAC_IDENTITY_REAC_VER_STR_CAP. Returns the string length written,
 * or <0 if the buffer is too small or `out` is NULL. Takes the three numbers
 * rather than the struct so a caller can format a bare version. */
int reac_identity_reac_ver_str(uint16_t major, uint16_t minor, uint16_t patch,
                               char *out, size_t cap);

#endif /* REAC_IDENTITY_H */
