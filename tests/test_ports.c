// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_ports — the config-announce port-table decode, pinned against the three
 * byte-verified model blocks (each captured from a real box; see reac_ports.h
 * for the capture provenance). The three blocks are copied VERBATIM — they are
 * the same bytes reac-pw's model matrix carries, which is the point: the matrix
 * NAMES a model, the table DECLARES its geometry, and both read the same wire
 * bytes. A slot code nobody has captured must refuse, never guess. */
#include <reac/reac_ports.h>
#include <reac/reac_ctrlblk.h>
#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* S-1608 (matrix-m200-s1608, 2026-07-11): 4 in-slots, 2 out, 6 empty. */
static const uint8_t BLK_S1608[32] = {
	0x01, 0x03, 0x00, 0x10, 0x82, 0x00, 0x00, 0x02,
	0x02, 0x02, 0x02, 0x02, 0x01, 0x01, 0x03, 0x03,
	0x03, 0x03, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c };

/* S-0808 (matrix-m200-s0808, 2026-07-11): 2 in, 2 out, 8 empty. */
static const uint8_t BLK_S0808[32] = {
	0x01, 0x03, 0x00, 0x10, 0x84, 0x00, 0x00, 0x00,
	0x02, 0x02, 0x01, 0x01, 0x03, 0x03, 0x03, 0x03,
	0x03, 0x03, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a };

/* S-4000S (s4000s-coldboot-m5000-2026-07-12; byte-identical from both units on
 * the M-200 and M-5000, re-verified 2026-08-20): 8 in, 2 out, 2 empty. */
static const uint8_t BLK_S4000S[32] = {
	0x01, 0x03, 0x00, 0x10, 0x84, 0x00, 0x00, 0x00,
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
	0x01, 0x01, 0x03, 0x03, 0x00, 0x03, 0x00, 0x00,
	0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c };

int main(void)
{
	struct reac_box_ports pt;

	/* The three real declarations decode to the six known width facts. */
	CHK(reac_ports_parse(BLK_S1608, &pt) == 0);
	CHK(pt.in_ch == 16 && pt.out_ch == 8);
	CHK(reac_ports_parse(BLK_S0808, &pt) == 0);
	CHK(pt.in_ch == 8 && pt.out_ch == 8);
	CHK(reac_ports_parse(BLK_S4000S, &pt) == 0);
	CHK(pt.in_ch == 32 && pt.out_ch == 8);

	/* Geometry needs no model row: an unnamed variant (a tail byte no matrix
	 * block carries) still declares 32x8 — dynamic detection over enumeration. */
	uint8_t unnamed[32];
	memcpy(unnamed, BLK_S4000S, 32);
	unnamed[26] ^= 0x5a;                      /* tail data, outside the table */
	CHK(reac_ports_parse(unnamed, &pt) == 0);
	CHK(pt.in_ch == 32 && pt.out_ch == 8);

	/* Not a config-announce block: refuse. WHAT MAKES IT ONE is the OPCODE at
	 * block[4], not the length at block[2:4] — this test used to shorten the
	 * length and call the result a heartbeat, which is the same mistake the
	 * parser made. Move the opcode and the block stops being a declaration; move
	 * the length and it is a declaration with a different body. */
	uint8_t wrong[32];
	memcpy(wrong, BLK_S0808, 32);
	wrong[4] = REAC_OP_SLOT_MAP;              /* the slot-record window */
	CHK(reac_ports_parse(wrong, &pt) == -1);
	memcpy(wrong, BLK_S0808, 32);
	wrong[0] = REAC_LINK_RECORD;              /* right opcode, wrong link */
	CHK(reac_ports_parse(wrong, &pt) == -1);
	memcpy(wrong, BLK_S0808, 32);
	wrong[1] = REAC_SEG_FIRST;                /* a fragment declares nothing */
	CHK(reac_ports_parse(wrong, &pt) == -1);
	memcpy(wrong, BLK_S0808, 32);
	wrong[3] = 0x19;                          /* only the LENGTH moved */
	CHK(reac_ports_parse(wrong, &pt) == 0);

	/* A slot code nobody has captured: refuse the whole table, never guess. */
	memcpy(wrong, BLK_S0808, 32);
	wrong[REAC_PORTS_TABLE_OFF + 5] = 0x04;
	CHK(reac_ports_parse(wrong, &pt) == -1);
	memcpy(wrong, BLK_S1608, 32);
	wrong[REAC_PORTS_TABLE_OFF + 0] = 0x00;
	CHK(reac_ports_parse(wrong, &pt) == -1);

	/* A refusal leaves the out-struct untouched — INCLUDING THE BASE. A block
	 * that does not parse is not a box, so nothing about it may be written:
	 * the caller's struct must come back exactly as it went in. */
	pt.in_ch = -7; pt.out_ch = -7; pt.headamp_base = -7;
	memcpy(wrong, BLK_S0808, 32);
	wrong[REAC_PORTS_TABLE_OFF] = 0xff;
	CHK(reac_ports_parse(wrong, &pt) == -1);
	CHK(pt.in_ch == -7 && pt.out_ch == -7 && pt.headamp_base == -7);

	CHK(reac_ports_parse(NULL, &pt) == -1);
	CHK(reac_ports_parse(BLK_S0808, NULL) == -1);

	/* ---- THE HEAD-AMP BASE IS THE ANNOUNCED STRAP -----------------------
	 *
	 * The three real declarations, read off the wire the way the firmware does
	 * it: base = block[7] * 0x10. */
	CHK(reac_ports_parse(BLK_S0808, &pt) == 0);
	CHK(pt.headamp_base == 0x00);          /* strap 0 */
	CHK(reac_ports_parse(BLK_S1608, &pt) == 0);
	CHK(pt.headamp_base == 0x20);          /* strap 2 */
	CHK(reac_ports_parse(BLK_S4000S, &pt) == 0);
	CHK(pt.headamp_base == 0x00);          /* strap 0 */

	/* THE GUARD, AND THE WHOLE POINT OF IT. Every assertion above passes just
	 * as well against the retired per-width table (8 -> 0x00, 16 -> 0x20,
	 * 32 -> 0x00), because width and strap are collinear on all three chassis
	 * we own. So the three above cannot tell the two derivations apart, and a
	 * suite made only of them is what let the width table survive.
	 *
	 * These break the collinearity. Each is a real declaration with its strap
	 * byte moved and NOTHING ELSE touched: the width the table would key on is
	 * unchanged, so the two derivations must now disagree, and the announced
	 * one has to win. If anyone puts the width table back, these go red and the
	 * ones above do not. */
	uint8_t strapped[32];

	memcpy(strapped, BLK_S1608, 32);       /* still 16 in / 8 out */
	strapped[REAC_HEADAMP_BASE_OFF] = 0x01;
	CHK(reac_ports_parse(strapped, &pt) == 0);
	CHK(pt.in_ch == 16);                   /* width unmoved ... */
	CHK(pt.headamp_base == 0x10);          /* ... base follows the STRAP */
	CHK(pt.headamp_base != 0x20);          /* which the width table would say */

	memcpy(strapped, BLK_S0808, 32);       /* still 8 in / 8 out */
	strapped[REAC_HEADAMP_BASE_OFF] = 0x02;
	CHK(reac_ports_parse(strapped, &pt) == 0);
	CHK(pt.in_ch == 8);
	CHK(pt.headamp_base == 0x20);
	CHK(pt.headamp_base != 0x00);          /* the width table's answer */

	memcpy(strapped, BLK_S4000S, 32);      /* still 32 in / 8 out */
	strapped[REAC_HEADAMP_BASE_OFF] = 0x03;
	CHK(reac_ports_parse(strapped, &pt) == 0);
	CHK(pt.in_ch == 32);
	CHK(pt.headamp_base == 0x30);
	CHK(pt.headamp_base != 0x00);

	/* Two boxes of DIFFERENT widths on the same strap land on the same base —
	 * the observation that rules out an allocation keyed on width. */
	uint8_t a[32], b[32];
	memcpy(a, BLK_S0808, 32);  a[REAC_HEADAMP_BASE_OFF] = 0x00;
	memcpy(b, BLK_S4000S, 32); b[REAC_HEADAMP_BASE_OFF] = 0x00;
	struct reac_box_ports pa, pb;
	CHK(reac_ports_parse(a, &pa) == 0 && reac_ports_parse(b, &pb) == 0);
	CHK(pa.in_ch == 8 && pb.in_ch == 32);
	CHK(pa.headamp_base == pb.headamp_base && pa.headamp_base == 0x00);

	/* The constants the schema binds. A row with no reader is a comment; these
	 * are the readers, and protocol-facts.yaml asserts against them. */
	CHK(REAC_HEADAMP_BASE_MULTIPLIER == 0x10);
	CHK(REAC_HEADAMP_BASE_OFF == 7);
	CHK(REAC_HEADAMP_BASE_FROM_CONFIG_BYTE7 == 1);
	CHK(REAC_HEADAMP_BASE_IS_CHASSIS_NOT_GRANT == 1);
	/* The APPLY unit — eight fabric rows per board. A DIFFERENT AXIS from
	 * actuation, which is per channel; neither corroborates the other. */
	CHK(REAC_HEADAMP_APPLY_UNIT_SLOTS == 8);

	printf("OK: reac_ports — three real declarations decode (16x8 / 8x8 / 32x8), "
	       "geometry needs no model row, uncaptured slot codes refuse; the "
	       "head-amp base is the ANNOUNCED strap x 0x10 and holds when width and "
	       "strap disagree\n");
	return 0;
}
