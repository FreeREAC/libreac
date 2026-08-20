// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The config-announce port-table decode. See reac_ports.h for the layout and
 * the three-model evidence trail. */
#include <reac/reac_ports.h>

int reac_ports_parse(const uint8_t block[32], struct reac_box_ports *out)
{
	if (!block || !out)
		return -1;
	if (block[0] != 0x01 || block[1] != 0x03 ||
	    block[2] != 0x00 || block[3] != 0x10)
		return -1;                       /* not a config-announce block */

	int in_slots = 0, out_slots = 0;
	for (int i = 0; i < REAC_PORTS_TABLE_SLOTS; i++) {
		switch (block[REAC_PORTS_TABLE_OFF + i]) {
		case REAC_PORT_SLOT_IN:    in_slots++;  break;
		case REAC_PORT_SLOT_OUT:   out_slots++; break;
		case REAC_PORT_SLOT_EMPTY: break;
		default:
			return -1;               /* a slot code nobody has captured */
		}
	}
	out->in_ch  = in_slots  * REAC_PORTS_CH_PER_SLOT;
	out->out_ch = out_slots * REAC_PORTS_CH_PER_SLOT;
	return 0;
}
