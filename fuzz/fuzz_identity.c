// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libFuzzer harness: the IDENTITY PAGE decode (src/reac_identity.c) and the box
 * naming it feeds (src/reac_box_facts.c).
 *
 * Input:  records, at most 64, each [u16 LE addr_lo][u8 len][len payload bytes]:
 *         the DT1 replies off tag 0x0500 a box sends one address at a time. The
 *         harness biases addr_lo onto the three addresses the decode models when
 *         the record's top bit pattern asks for it (see pick_addr), so the
 *         fuzzer does not have to guess 16-bit constants.
 *         Then a tail: [family][in_ch][out_ch][cap_fw][cap_ver][cap_name]
 *         [cap_token][cap_display][catalogue row][u16 LE any firmware value].
 *
 * Every payload and every output buffer is its own allocation of exactly the
 * length or capacity the input chose, so a read or write past it is a sanitizer
 * report rather than a quiet stray byte.
 *
 * Properties, each an abort():
 *   - an accepted firmware reply is four decimal digits, and reac_identity_fw_str
 *     prints exactly those digits as D.DDD;
 *   - an accepted model name is the payload's ASCII up to its first NUL, never
 *     empty, NUL-terminated inside the struct;
 *   - an accepted REAC-version record keeps the raw eight bytes and decodes the
 *     last three words big-endian;
 *   - every formatter either refuses or writes a NUL-terminated string inside the
 *     capacity it was given, of the length it returns;
 *   - reac_box_name's three strings agree: the token is the name lower-cased with
 *     its first '-' dropped, and the display starts with the name.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <reac/reac.h>
#include <reac/reac_box_facts.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_identity.h>

#include "fuzz.h"

#define MAX_RECORDS 64

static uint16_t pick_addr(uint16_t raw)
{
	switch (raw >> 14) {
	case 0:  return REAC_IDENTITY_ADDR_FIRMWARE;
	case 1:  return REAC_IDENTITY_ADDR_REAC_VERSION;
	case 2:  return REAC_IDENTITY_ADDR_MODEL_NAME;
	default: return raw;   /* any address at all, modelled or not */
	}
}

static char *buf_of(size_t cap)
{
	char *b = malloc(cap ? cap : 1);
	if (!b)
		abort();
	return b;
}

/* A formatter's output: refused (<0), or a string of length r inside cap. */
static void check_str(int r, const char *out, size_t cap)
{
	if (r < 0)
		return;
	if ((size_t)r >= cap || out[r] != '\0' || strlen(out) != (size_t)r)
		abort();
}

static void ingest(struct reac_identity *id, uint16_t addr, const uint8_t *pl, size_t n)
{
	struct reac_identity before = *id;
	int r = reac_identity_ingest(id, addr, pl, n);
	if (r < 0)
		abort();   /* neither argument was NULL */
	if (r == 0) {
		/* An ignored record changes nothing. */
		if (memcmp(&before, id, sizeof before) != 0)
			abort();
		return;
	}
	switch (addr) {
	case REAC_IDENTITY_ADDR_FIRMWARE: {
		if (n < 4 || !id->has_fw)
			abort();
		for (int i = 0; i < 4; i++)
			if (pl[i] > 9)
				abort();
		if (id->fw_milli != pl[0] * 1000 + pl[1] * 100 + pl[2] * 10 + pl[3])
			abort();
		char s[REAC_IDENTITY_FW_STR_CAP];
		int w = reac_identity_fw_str(id->fw_milli, s, sizeof s);
		check_str(w, s, sizeof s);
		if (w != 5 || s[0] != '0' + pl[0] || s[1] != '.' || s[2] != '0' + pl[1] ||
		    s[3] != '0' + pl[2] || s[4] != '0' + pl[3])
			abort();
		break;
	}
	case REAC_IDENTITY_ADDR_MODEL_NAME: {
		const char *nul = memchr(id->model_name, 0, sizeof id->model_name);
		if (!id->has_model_name || !nul)
			abort();
		size_t w = (size_t)(nul - id->model_name);
		if (w == 0)
			abort();
		if (n < 1 + w || memcmp(id->model_name, pl + 1, w) != 0)
			abort();
		if (n > 1 + w && w < REAC_IDENTITY_NAME_CAP - 1 && pl[1 + w] != 0)
			abort();   /* stopped early without a NUL to stop at */
		break;
	}
	case REAC_IDENTITY_ADDR_REAC_VERSION:
		if (n < REAC_IDENTITY_REAC_VER_LEN || !id->has_reac_version ||
		    memcmp(id->reac_version_raw, pl, REAC_IDENTITY_REAC_VER_LEN) != 0 ||
		    id->reac_version_major != (uint16_t)(pl[2] << 8 | pl[3]) ||
		    id->reac_version_minor != (uint16_t)(pl[4] << 8 | pl[5]) ||
		    id->reac_version_patch != (uint16_t)(pl[6] << 8 | pl[7]))
			abort();
		break;
	default:
		abort();   /* an unmodelled address was accepted */
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size >= FUZZ_INPUT_MAX)
		return 0;
	struct fuzz_in in = fuzz_in(data, size);
	struct reac_identity id;
	reac_identity_init(NULL);   /* a NULL identity is ignored, not dereferenced */
	reac_identity_init(&id);
	if (reac_identity_ingest(NULL, 0, NULL, 0) != -1 ||
	    reac_identity_ingest(&id, 0, NULL, 4) != -1 ||
	    reac_identity_fw_str(0, NULL, 8) >= 0 || reac_identity_reac_ver_str(0, 0, 0, NULL, 20) >= 0 ||
	    reac_box_catalogue_defect(NULL, 8, 8, "x") != 0 || reac_box_family_of(NULL) != REAC_BOX_FAMILY_UNKNOWN)
		abort();

	int nrec = fuzz_u8(&in) % (MAX_RECORDS + 1);
	for (int i = 0; i < nrec; i++) {
		uint16_t addr = pick_addr(fuzz_u16(&in));
		const uint8_t *body;
		size_t n = fuzz_take(&in, fuzz_u8(&in), &body);
		uint8_t *pl = fuzz_dup(body, n);
		ingest(&id, addr, pl, n);
		free(pl);
	}

	uint8_t fam_raw = fuzz_u8(&in);
	int in_ch = fuzz_u8(&in), out_ch = fuzz_u8(&in);
	size_t cap_fw = fuzz_u8(&in) % 16, cap_ver = fuzz_u8(&in) % 32;
	size_t cap_name = fuzz_u8(&in) % 40, cap_tok = fuzz_u8(&in) % 40,
	       cap_disp = fuzz_u8(&in) % 64;
	uint8_t row = fuzz_u8(&in);
	uint16_t any_fw = fuzz_u16(&in);

	/* The formatters at an input-chosen capacity, on the decoded firmware and on any
	 * value at all (a two-digit whole part is carried, not truncated). */
	char *s = buf_of(cap_fw);
	check_str(reac_identity_fw_str(id.fw_milli, s, cap_fw), s, cap_fw);
	check_str(reac_identity_fw_str(any_fw, s, cap_fw), s, cap_fw);
	free(s);
	s = buf_of(cap_ver);
	check_str(reac_identity_reac_ver_str(id.reac_version_major, id.reac_version_minor,
	                                     id.reac_version_patch, s, cap_ver), s, cap_ver);
	free(s);

	/* The family is the identity page's verdict, or any value the input names. */
	enum reac_box_family fam = (fam_raw & 0x80) ? (enum reac_box_family)(fam_raw & 0x7f)
	                                            : reac_box_family_of(&id);
	if (!reac_box_family_stem(fam))
		abort();
	const char *said = id.has_model_name ? id.model_name : NULL;
	char *nm = buf_of(cap_name), *tk = buf_of(cap_tok), *dp = buf_of(cap_disp);
	if (reac_box_name(fam, said, 8, 8, NULL, 8, tk, cap_tok, dp, cap_disp) != -1)
		abort();
	int r = reac_box_name(fam, said, in_ch, out_ch, nm, cap_name, tk, cap_tok, dp, cap_disp);
	if (r != 0 && r != -1)
		abort();
	if (r == 0) {
		check_str((int)strlen(nm), nm, cap_name);
		check_str((int)strlen(tk), tk, cap_tok);
		check_str((int)strlen(dp), dp, cap_disp);
		/* token = name lower-cased, its first '-' dropped */
		size_t j = 0;
		int dropped = 0;
		for (size_t i = 0; nm[i]; i++) {
			char c = nm[i];
			if (c == '-' && !dropped) { dropped = 1; continue; }
			if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
			if (tk[j++] != c)
				abort();
		}
		if (tk[j] != '\0' || strncmp(dp, nm, strlen(nm)) != 0)
			abort();
		size_t count = 0;
		const struct reac_box_model *cat = reac_box_catalogue(&count);
		int mask = reac_box_catalogue_defect(count ? &cat[row % count] : NULL,
		                                     in_ch, out_ch, dp);
		if (mask & ~(REAC_BOX_DEFECT_WIDTH | REAC_BOX_DEFECT_NAME))
			abort();
	}
	free(dp);
	free(tk);
	free(nm);
	return 0;
}
