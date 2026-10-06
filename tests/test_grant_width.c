// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* A BOX IS GRANTED AT ANY WIDTH IT CAN DECLARE (operator ruling 2026-10-06: a REAC
 * frame carries 40 channels, and a box declares its inputs and outputs in multiples of
 * four, in any combination).
 *
 * The desk, 2026-10-06: a 40 in / 0 out box (00:40:ab:c4:06:80) declared cells
 * 02 x10, 03 x2 and looped CONFIG -> GRANTING -> PROBING every ~5 s, because the
 * grant refused any width past 32 and reac_master_set_box then applied nothing.
 *
 *   40 / 0   the declaration parses 40 / 0 and the master holds a 40-wide grant at
 *            the announced base 0x00, with the full 8 + 40 x 3 = 128-row sweep;
 *   24 / 16  a second split no stock model has, granted 24 wide;
 *   32 / 8   the stock S-4000S width, unchanged (104 rows). */
#include <reac/reac.h>
#include <reac/reac_ports.h>
#include <reac/reac_grant.h>
#include <reac/reac_master.h>

#include <stdio.h>
#include <string.h>

static int fails;
#define CHK(c) do { \
	if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
} while (0)

static const uint8_t OUR[6] = { 0x00, 0x14, 0x5c, 0x9b, 0x28, 0x2d };

/* A config-announce control block (link 1, SINGLE, length 0x10, opcode 0x84, strap
 * 0x00) with `in` input cells and `out` output cells, the rest empty. */
static void decl(uint8_t blk[32], int in, int out)
{
	memset(blk, 0, 32);
	blk[0] = 0x01; blk[1] = 0x03; blk[2] = 0x00; blk[3] = 0x10; blk[4] = 0x84;
	int c = 0;
	for (int i = 0; i < in; i++)  blk[REAC_PORTS_TABLE_OFF + c++] = REAC_PORT_SLOT_IN;
	for (int i = 0; i < out; i++) blk[REAC_PORTS_TABLE_OFF + c++] = REAC_PORT_SLOT_OUT;
	while (c < REAC_PORTS_TABLE_SLOTS) blk[REAC_PORTS_TABLE_OFF + c++] = REAC_PORT_SLOT_EMPTY;
}

static void granted(int in_cells, int out_cells, int want_in, int want_out)
{
	static struct reac_master m;
	uint8_t blk[32];
	struct reac_box_ports ports;
	decl(blk, in_cells, out_cells);
	CHK(reac_ports_parse(blk, &ports) == 0);
	CHK(ports.in_ch == want_in && ports.out_ch == want_out);
	reac_master_init(&m, OUR, NULL, 8000);
	reac_master_set_box(&m, ports.in_ch, ports.out_ch, ports.headamp_base);
	CHK(m.alloc.width == want_in);
	CHK(m.alloc.base == 0x00);
	CHK(m.grant_burst_len == REAC_GRANT_SWEEP_LEN(want_in));
}

int main(void)
{
	CHK(REAC_GRANT_MAX_WIDTH == 40);
	granted(10, 0, 40, 0);   /* 02 x10, 03 x2 */
	granted(6, 4, 24, 16);
	granted(8, 2, 32, 8);
	if (fails) {
		fprintf(stderr, "test_grant_width: %d failure(s)\n", fails);
		return 1;
	}
	printf("OK: a 40 in / 0 out declaration is granted 40 wide (128-row sweep), a 24 / 16 "
	       "one 24 wide, and the S-4000S's 32 / 8 is unchanged\n");
	return 0;
}
