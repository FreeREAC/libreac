// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_box_facts — what a CONNECTED box is, read from its own frames.
 *
 * Operator ruling 2026-10-07: every fact about a connected box comes from the
 * protocol. Two frames carry them between them:
 *
 *   config announce   the declared widths, both directions (reac_ports_parse)
 *   identity page     the firmware (0x0000), the hw block (0x0600) and, on the
 *                     boxes that send one, the name (0x1000) (reac_identity.h)
 *
 * The hw block names the FAMILY (reac-protocol reac.ksy, identity_data.box_family);
 * the family and the widths name the model (reac.ksy, type box_model). Nothing
 * here reads the model catalogue: that is for emulation and offline planning
 * (reac_ctrlblk.h), and reac_box_catalogue_defect below only COMPARES with it.
 *
 * Pure, kernel-portable: no allocation, no stdio. */
#ifndef REAC_BOX_FACTS_H
#define REAC_BOX_FACTS_H

#include <stddef.h>
#include <stdint.h>

#include <reac/reac_identity.h>

struct reac_box_model;   /* reac_ctrlblk.h — a catalogue entry */

/* The families the hw block names. Values match reac.ksy's `box_family`. */
enum reac_box_family {
	REAC_BOX_FAMILY_UNKNOWN = 0,
	REAC_BOX_FAMILY_S0808   = 1,   /* hw block 00000001 00000000 */
	REAC_BOX_FAMILY_S1608   = 2,   /* hw block 00000002 00030002 */
	REAC_BOX_FAMILY_S4000S  = 3,   /* hw block 00000002 00010002 */
};

/* THE ONE RECOGNITION RULE. The S-1608 and the S-4000S never send their name, so
 * the text is not on the wire; their hw blocks tell the three captured families
 * apart, one value each (the table is reac.ksy's, with its captures). Any other
 * value, or no 0x0600 reply yet, is UNKNOWN: never the nearest family. */
enum reac_box_family reac_box_family_of(const struct reac_identity *id);

/* "S-0808", "S-1608", "S-4000S", or "REAC" for UNKNOWN. */
const char *reac_box_family_stem(enum reac_box_family f);

/* THE MODEL NAME, derived (reac.ksy `box_model`). The stem is the name the box
 * SENT on its identity page when it sent one (the S-0808 does), else the family's
 * stem. Then `-` and the declared inputs and outputs as two two-digit numbers,
 * unless the stem already ends in exactly those four digits:
 *
 *   S-1608 16/8 -> "S-1608"          S-4000S 32/8  -> "S-4000S-3208"
 *   S-0808 8/8  -> "S-0808"          S-4000S 16/24 -> "S-4000S-1624"
 *   S-1608 8/8  -> "S-1608-0808"     S-4000S 40/0  -> "S-4000S-4000"
 *   unknown 8/16 -> "REAC-0816"
 *
 * `token` is the name lower-cased with its first '-' dropped ("s1608",
 * "s4000s-1624", "reac0816"); `display` is "<name> (<in> in / <out> out)".
 * `said_name` may be NULL or "". Returns 0, or -1 (writing nothing) when a width
 * is not 0 or a box width, both are 0, or a buffer is too small
 * (REAC_BOX_NAME_TOKEN_MAX / REAC_BOX_NAME_DISPLAY_MAX always fit). */
#define REAC_BOX_NAME_MAX         24
#define REAC_BOX_NAME_TOKEN_MAX   24
#define REAC_BOX_NAME_DISPLAY_MAX 48
int reac_box_name(enum reac_box_family f, const char *said_name, int in_ch, int out_ch,
                  char *name, size_t name_len, char *token, size_t token_len,
                  char *display, size_t display_len);

/* A CATALOGUE DEFECT: the entry a declaration matched (reac_box_catalogue_match)
 * says something other than the box. Returns a mask, 0 when they agree or there is
 * no entry: REAC_BOX_DEFECT_WIDTH when its widths differ from the declared ones,
 * REAC_BOX_DEFECT_NAME when its display differs from the derived `display`. The
 * box wins either way; the mask is what a binding logs. */
#define REAC_BOX_DEFECT_WIDTH 1
#define REAC_BOX_DEFECT_NAME  2
int reac_box_catalogue_defect(const struct reac_box_model *entry, int in_ch, int out_ch,
                              const char *display);

#endif /* REAC_BOX_FACTS_H */
