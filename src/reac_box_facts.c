// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* What a connected box is, read from its frames. See reac_box_facts.h. */
#include <reac/reac_box_facts.h>
#include <reac/reac.h>          /* reac_box_width_ok */
#include <reac/reac_ctrlblk.h>  /* struct reac_box_model */

#include <string.h>

/* The hw blocks of reac.ksy's family table, as the 0x0600 record carries them. */
static const struct {
	enum reac_box_family f;
	uint8_t hw[REAC_IDENTITY_REAC_VER_LEN];
} HW_FAMILY[] = {
	{ REAC_BOX_FAMILY_S0808,  { 0, 0, 0, 1, 0, 0, 0, 0 } },
	{ REAC_BOX_FAMILY_S1608,  { 0, 0, 0, 2, 0, 3, 0, 2 } },
	{ REAC_BOX_FAMILY_S4000S, { 0, 0, 0, 2, 0, 1, 0, 2 } },
};

enum reac_box_family reac_box_family_of(const struct reac_identity *id)
{
	if (!id || !id->has_reac_version)
		return REAC_BOX_FAMILY_UNKNOWN;
	for (size_t i = 0; i < sizeof HW_FAMILY / sizeof HW_FAMILY[0]; i++)
		if (memcmp(id->reac_version_raw, HW_FAMILY[i].hw, sizeof HW_FAMILY[i].hw) == 0)
			return HW_FAMILY[i].f;
	return REAC_BOX_FAMILY_UNKNOWN;
}

const char *reac_box_family_stem(enum reac_box_family f)
{
	switch (f) {
	case REAC_BOX_FAMILY_S0808:  return "S-0808";
	case REAC_BOX_FAMILY_S1608:  return "S-1608";
	case REAC_BOX_FAMILY_S4000S: return "S-4000S";
	default:                     return "REAC";
	}
}

/* A bounded string builder: no stdio in this file. */
struct sb { char *p; size_t cap, n; int over; };

static void sb_ch(struct sb *b, char c)
{
	if (b->n + 1 >= b->cap) { b->over = 1; return; }
	b->p[b->n++] = c;
}

static void sb_str(struct sb *b, const char *s)
{
	for (; *s; s++)
		sb_ch(b, *s);
}

static void sb_dec(struct sb *b, int v, int min_digits)
{
	char d[4];
	int k = 0;
	do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 3);
	while (k < min_digits && k < 3) d[k++] = '0';
	while (k)
		sb_ch(b, d[--k]);
}

static size_t ends_with_widths(const char *stem, const char *w4)
{
	size_t n = strlen(stem);
	return n >= 4 && memcmp(stem + n - 4, w4, 4) == 0;
}

int reac_box_name(enum reac_box_family f, const char *said_name, int in_ch, int out_ch,
                  char *name, size_t name_len, char *token, size_t token_len,
                  char *display, size_t display_len)
{
	if (!name || !token || !display || !name_len || !token_len || !display_len)
		return -1;
	if ((in_ch != 0 && !reac_box_width_ok(in_ch)) ||
	    (out_ch != 0 && !reac_box_width_ok(out_ch)) || (in_ch == 0 && out_ch == 0))
		return -1;

	const char *stem = (said_name && said_name[0]) ? said_name : reac_box_family_stem(f);

	char w4[5];
	struct sb wb = { w4, sizeof w4, 0, 0 };
	sb_dec(&wb, in_ch, 2);
	sb_dec(&wb, out_ch, 2);
	w4[wb.n] = 0;

	char nm[REAC_BOX_NAME_MAX];
	struct sb nb = { nm, sizeof nm, 0, 0 };
	sb_str(&nb, stem);
	if (!(wb.n == 4 && ends_with_widths(stem, w4))) {
		sb_ch(&nb, '-');
		sb_str(&nb, w4);
	}
	nm[nb.n] = 0;

	char tk[REAC_BOX_NAME_TOKEN_MAX];
	struct sb tb = { tk, sizeof tk, 0, 0 };
	int dropped = 0;
	for (size_t i = 0; i < nb.n; i++) {
		char c = nm[i];
		if (c == '-' && !dropped) { dropped = 1; continue; }
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		sb_ch(&tb, c);
	}
	tk[tb.n] = 0;

	char dp[REAC_BOX_NAME_DISPLAY_MAX];
	struct sb db = { dp, sizeof dp, 0, 0 };
	sb_str(&db, nm);
	sb_str(&db, " (");
	sb_dec(&db, in_ch, 1);
	sb_str(&db, " in / ");
	sb_dec(&db, out_ch, 1);
	sb_str(&db, " out)");
	dp[db.n] = 0;

	if (wb.over || nb.over || tb.over || db.over ||
	    nb.n >= name_len || tb.n >= token_len || db.n >= display_len)
		return -1;
	memcpy(name, nm, nb.n + 1);
	memcpy(token, tk, tb.n + 1);
	memcpy(display, dp, db.n + 1);
	return 0;
}

int reac_box_catalogue_defect(const struct reac_box_model *entry, int in_ch, int out_ch,
                              const char *display)
{
	if (!entry)
		return 0;
	int mask = 0;
	if (entry->in_ch != in_ch || entry->out_ch != out_ch)
		mask |= REAC_BOX_DEFECT_WIDTH;
	if (display && entry->display && strcmp(entry->display, display) != 0)
		mask |= REAC_BOX_DEFECT_NAME;
	return mask;
}
