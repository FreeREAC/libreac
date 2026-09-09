// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_link — the REAC control plane as one entry point. See <reac/reac_link.h>. */
#include <reac/reac_link.h>
#include <string.h>

/* The three descriptor states a joining box's FILLERS carry, and the one rule about them
 * that a byte diff of 40 001 frames found: zero before the announce, REQUESTING from the
 * announce until the grant, ESTABLISHED after. */
void reac_link_fill_descriptor(uint8_t *frame, enum reac_link_desc d)
{
	if (d == REAC_LINK_DESC_NONE) {
		memset(frame + REAC_CTRL_BLOCK_OFF, 0, 32);
		return;
	}
	for (int i = 0; i < 16; i++) {
		frame[REAC_CTRL_BLOCK_OFF + i * 2]     = 0x00;
		frame[REAC_CTRL_BLOCK_OFF + i * 2 + 1] = (uint8_t)d;
	}
}

enum reac_link_desc reac_link_desc_for(enum reac_fsm_state st, int announced)
{
	if (st == FSM_ESTABLISHED)
		return REAC_LINK_DESC_ESTABLISHED;
	if (announced)
		return REAC_LINK_DESC_REQUESTING;
	return REAC_LINK_DESC_NONE;
}

int reac_link_slave_burst(int step, uint8_t *out, const uint8_t master[6],
                          const uint8_t src[6], uint16_t counter, int n_ch,
                          float *const *planar, int ns)
{
	switch (step) {
	case 0:
		return (int)reac_ctrl_build_coldconnect(out, master, src, counter, n_ch,
		                                        planar, ns);
	case 1:
		return (int)reac_ctrl_build_coldconnect_0013(out, master, src, counter, n_ch,
		                                             planar, ns);
	case 2:
		return (int)reac_ctrl_build_box_hb(out, master, src, counter, n_ch);
	default:
		return 0;
	}
}
