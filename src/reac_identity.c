// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Runtime decode of the REAC identity page (DT1 tag 0x0500). See
 * include/reac/reac_identity.h and reac-protocol/spec/reac.ksy `identity_data`
 * for the address map and the evidence behind each resolved field. */

#include "reac/reac_identity.h"

#include <string.h>

void reac_identity_init(struct reac_identity *id)
{
	if (!id)
		return;
	memset(id, 0, sizeof(*id));
}

/* The four firmware bytes are four decimal digits, most significant first
 * (reac.ksy: payload[0]*1000 + payload[1]*100 + payload[2]*10 + payload[3]).
 * A byte outside 0..9 is not a clean version and the whole reply is refused —
 * a malformed frame must not surface as a plausible-looking firmware number. */
static int firmware_milli(const uint8_t *p, size_t len, uint16_t *out)
{
	if (len < 4)
		return 0;
	for (size_t i = 0; i < 4; i++)
		if (p[i] > 9)
			return 0;
	*out = (uint16_t)(p[0] * 1000 + p[1] * 100 + p[2] * 10 + p[3]);
	return 1;
}

/* The model-name payload is name_kind (0x01) then up to 16 NUL-padded ASCII
 * bytes. Copy the ASCII, stop at the first NUL, and refuse a payload that is
 * only the kind byte with no name (a reply shorter than the console asked for
 * can carry nothing, and an empty name is not a name). */
static int store_model_name(struct reac_identity *id, const uint8_t *p, size_t len)
{
	if (len < 2)
		return 0;
	size_t n = len - 1;               /* drop the name_kind byte */
	if (n > REAC_IDENTITY_NAME_CAP - 1)
		n = REAC_IDENTITY_NAME_CAP - 1;
	size_t w = 0;
	for (size_t i = 0; i < n; i++) {
		if (p[1 + i] == 0x00)
			break;
		id->model_name[w++] = (char)p[1 + i];
	}
	if (w == 0)
		return 0;
	id->model_name[w] = '\0';
	id->has_model_name = 1;
	return 1;
}

/* The 0x0600 record is four u16be words: a reserved word then major, minor and
 * patch. Every byte pair is read big-endian, the order the wire carries. */
static uint16_t u16be(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

int reac_identity_ingest(struct reac_identity *id, uint16_t addr_lo,
                         const uint8_t *payload, size_t len)
{
	if (!id || (len > 0 && !payload))
		return -1;

	switch (addr_lo) {
	case REAC_IDENTITY_ADDR_FIRMWARE: {
		uint16_t milli;
		if (!firmware_milli(payload, len, &milli))
			return 0;
		id->fw_milli = milli;
		id->has_fw = 1;
		return 1;
	}
	case REAC_IDENTITY_ADDR_MODEL_NAME:
		return store_model_name(id, payload, len);
	case REAC_IDENTITY_ADDR_REAC_VERSION:
		if (len < REAC_IDENTITY_REAC_VER_LEN)
			return 0;
		memcpy(id->reac_version_raw, payload, REAC_IDENTITY_REAC_VER_LEN);
		id->reac_version_major = u16be(payload + 2);
		id->reac_version_minor = u16be(payload + 4);
		id->reac_version_patch = u16be(payload + 6);
		id->has_reac_version = 1;
		return 1;
	default:
		return 0;
	}
}

int reac_identity_fw_str(uint16_t fw_milli, char *out, size_t cap)
{
	if (!out || cap < REAC_IDENTITY_FW_STR_CAP)
		return -1;
	unsigned whole = (unsigned)(fw_milli / 1000u);
	unsigned frac = (unsigned)(fw_milli % 1000u);
	/* D.DDD — whole is one digit for every version in the corpus, but the
	 * arithmetic below carries a two-digit whole part too rather than truncate. */
	int w = 0;
	if (whole >= 10)
		out[w++] = (char)('0' + (whole / 10) % 10);
	out[w++] = (char)('0' + whole % 10);
	out[w++] = '.';
	out[w++] = (char)('0' + (frac / 100) % 10);
	out[w++] = (char)('0' + (frac / 10) % 10);
	out[w++] = (char)('0' + frac % 10);
	out[w] = '\0';
	return w;
}

/* Decimal, no padding, no libc. Returns the digits written. A uint16_t is at
 * most five digits, which is what REAC_IDENTITY_REAC_VER_STR_CAP budgets for. */
static int write_uint(char *out, uint16_t v)
{
	char tmp[5];
	int n = 0;
	do {
		tmp[n++] = (char)('0' + v % 10u);
		v = (uint16_t)(v / 10u);
	} while (v);
	for (int i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i];
	return n;
}

int reac_identity_reac_ver_str(uint16_t major, uint16_t minor, uint16_t patch,
                               char *out, size_t cap)
{
	if (!out || cap < REAC_IDENTITY_REAC_VER_STR_CAP)
		return -1;
	/* `major.minorPP` — what the M-200i prints: the minor and the patch run
	 * together with the patch zero-padded to two digits, so (2,3,2) is "2.302".
	 * A patch of 100 or more (never seen) widens rather than truncating, because
	 * a wrong version is worse than a long one. */
	int w = 0;
	w += write_uint(out + w, major);
	out[w++] = '.';
	w += write_uint(out + w, minor);
	if (patch < 10)
		out[w++] = '0';
	w += write_uint(out + w, patch);
	out[w] = '\0';
	return w;
}
