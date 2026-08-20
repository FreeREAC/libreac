// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_ports — the config-announce port-table decode, pinned against the three
 * byte-verified model blocks (each captured from a real box; see reac_ports.h
 * for the capture provenance). The three blocks are copied VERBATIM — they are
 * the same bytes reac-pw's model matrix carries, which is the point: the matrix
 * NAMES a model, the table DECLARES its geometry, and both read the same wire
 * bytes. A slot code nobody has captured must refuse, never guess. */
#include <reac/reac_ports.h>
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

	/* Not a config-announce block: refuse. */
	uint8_t wrong[32];
	memcpy(wrong, BLK_S0808, 32);
	wrong[3] = 0x19;                          /* 01 03 0019 = master HB, not config */
	CHK(reac_ports_parse(wrong, &pt) == -1);

	/* A slot code nobody has captured: refuse the whole table, never guess. */
	memcpy(wrong, BLK_S0808, 32);
	wrong[REAC_PORTS_TABLE_OFF + 5] = 0x04;
	CHK(reac_ports_parse(wrong, &pt) == -1);
	memcpy(wrong, BLK_S1608, 32);
	wrong[REAC_PORTS_TABLE_OFF + 0] = 0x00;
	CHK(reac_ports_parse(wrong, &pt) == -1);

	/* A refusal leaves the out-struct untouched. */
	pt.in_ch = -7; pt.out_ch = -7;
	memcpy(wrong, BLK_S0808, 32);
	wrong[REAC_PORTS_TABLE_OFF] = 0xff;
	CHK(reac_ports_parse(wrong, &pt) == -1);
	CHK(pt.in_ch == -7 && pt.out_ch == -7);

	CHK(reac_ports_parse(NULL, &pt) == -1);
	CHK(reac_ports_parse(BLK_S0808, NULL) == -1);

	/* Head-amp placement per declared width — the corpus law (42 grant sweeps,
	 * reac-captures docs/PLACEMENT-EVIDENCE.md): every desk generation grants
	 * the same base for the same declaration. Unknown width REFUSES (-1):
	 * guessing base 0 addresses an S-1608's preamps 32 slots low. */
	CHK(reac_headamp_base(8) == 0x00);
	CHK(reac_headamp_base(16) == 0x20);
	CHK(reac_headamp_base(32) == 0x00);
	CHK(reac_headamp_base(24) == -1);
	CHK(reac_headamp_base(0) == -1);
	CHK(reac_headamp_base(-4) == -1);

	printf("OK: reac_ports — three real declarations decode (16x8 / 8x8 / 32x8), "
	       "geometry needs no model row, uncaptured slot codes refuse\n");
	return 0;
}
