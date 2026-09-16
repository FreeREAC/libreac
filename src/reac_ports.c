// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The config-announce port-table decode. See reac_ports.h for the layout and
 * the three-model evidence trail. */
#include <reac/reac_ports.h>
#include <reac/reac_ctrlblk.h>   /* REAC_LINK_CTRL, the declaration opcodes */

int reac_ports_parse(const uint8_t block[32], struct reac_box_ports *out)
{
	if (!block || !out)
		return -1;
	/* THE DISCRIMINATOR IS THE OPCODE AT block[4], not block[2:4]. Those two
	 * bytes are a LENGTH, and a declaration happens to be 0x10 long, so testing
	 * it appears to work and refuses any declaration whose body is not that
	 * size. block[0] is the link and block[1] the segment bits. */
	if (block[0] != REAC_LINK_CTRL || block[1] != REAC_SEG_SINGLE)
		return -1;
	if (block[4] != REAC_OP_DECL && block[4] != REAC_OP_DECL_ALT &&
	    block[4] != REAC_OP_DECL_OTHER)
		return -1;                       /* not a config-announce block */

	int in_slots = 0, out_slots = 0;
	for (int i = 0; i < REAC_PORTS_TABLE_SLOTS; i++) {
		uint8_t code = block[REAC_PORTS_TABLE_OFF + i];
		switch (code) {
		case REAC_PORT_SLOT_IN:
		case REAC_PORT_SLOT_IN_SPLIT:
			in_slots++;  break;
		case REAC_PORT_SLOT_OUT:   out_slots++; break;
		case REAC_PORT_SLOT_EMPTY: break;
		default:
			/* A code nobody has captured is FOUR CHANNELS we cannot place —
			 * not a reason to refuse the eleven slots we can read. See the
			 * header: the box enrols at the width we understand, and
			 * reac_ports_unknown says what was left out. */
			break;
		}
	}
	out->in_ch  = in_slots  * REAC_PORTS_CH_PER_SLOT;
	out->out_ch = out_slots * REAC_PORTS_CH_PER_SLOT;
	/* THE BASE COMES OFF THE WIRE, from the chassis strap the box announces —
	 * never from in_ch, which agrees only by a collinearity the next chassis is
	 * free to break. See reac_ports.h for the firmware and corpus provenance. */
	out->headamp_base = block[REAC_HEADAMP_BASE_OFF] * REAC_HEADAMP_BASE_MULTIPLIER;
	return 0;
}

int reac_ports_unknown(const uint8_t block[32], uint8_t *first_code)
{
	if (first_code)
		*first_code = 0;
	if (!block)
		return -1;
	if (block[0] != REAC_LINK_CTRL || block[1] != REAC_SEG_SINGLE)
		return -1;
	if (block[4] != REAC_OP_DECL && block[4] != REAC_OP_DECL_ALT &&
	    block[4] != REAC_OP_DECL_OTHER)
		return -1;
	int unknown = 0;
	for (int i = 0; i < REAC_PORTS_TABLE_SLOTS; i++) {
		uint8_t code = block[REAC_PORTS_TABLE_OFF + i];
		if (code == REAC_PORT_SLOT_IN || code == REAC_PORT_SLOT_IN_SPLIT ||
		    code == REAC_PORT_SLOT_OUT || code == REAC_PORT_SLOT_EMPTY)
			continue;
		if (unknown == 0 && first_code)
			*first_code = code;
		unknown++;
	}
	return unknown * REAC_PORTS_CH_PER_SLOT;
}
