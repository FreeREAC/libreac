// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_ports — the box's DECLARED port table, decoded from its config-announce.
 *
 * A stagebox declares its own geometry in the sync protocol: the config-announce
 * (cdea, link 1, SINGLE, opcode 0x82 / 0x84 / 0x80) carries a PORT TABLE at
 * block[8..19] — twelve
 * slots, one byte per 4-channel group, spanning the 48-channel REAC fabric ring
 * (12 x 4 = 48, the same ring the master's channel-map sweep walks):
 *
 *     0x02  4-input group
 *     0x01  4-output group
 *     0x03  empty slot
 *
 * Evidence — the three byte-verified model blocks, each captured from a real
 * box (matrix-m200-s1608 / -s0808 2026-07-11; S-4000S from s4000s-coldboot-
 * m5000-2026-07-12 and matrix-m200-s4000-coldconnect-2026-07-24, both units
 * byte-identical):
 *
 *     S-1608   02 x4, 01 x2, 03 x6   -> 16 in / 8 out   (4+2+6 = 12)
 *     S-0808   02 x2, 01 x2, 03 x8   ->  8 in / 8 out   (2+2+8 = 12)
 *     S-4000S  02 x8, 01 x2, 03 x2   -> 32 in / 8 out   (8+2+2 = 12)
 *
 * All six width facts reproduce and every table sums to exactly 12 slots. The
 * bytes BEFORE the table (block[4..7]) are the model-family selector + flags
 * (0x82 / 0x84 ...), and the bytes after it are model tail data — neither is
 * needed for geometry, which is the point: geometry comes from the declaration,
 * not from a hand-kept model list. A model NAME is still a byte-exact matrix
 * match (reac-pw's recognizer); an unnamed box still declares its widths here.
 *
 * A slot code outside {01, 02, 03} is a table nobody has captured — REFUSE
 * (-1), never guess: a mis-read geometry reaches the wire as a wrong-width
 * grant. The caller validates the frame around the block (0x8819, cdea type,
 * checksum) — this is a pure block decode, no frame concerns. */
#ifndef REAC_PORTS_H
#define REAC_PORTS_H

#include <stdint.h>

#define REAC_PORTS_TABLE_OFF   8   /* table start, block-relative           */
#define REAC_PORTS_TABLE_SLOTS 12  /* 12 slots x 4 ch = the 48-ch fabric    */
#define REAC_PORTS_CH_PER_SLOT 4

#define REAC_PORT_SLOT_OUT   0x01
#define REAC_PORT_SLOT_IN    0x02
#define REAC_PORT_SLOT_EMPTY 0x03

struct reac_box_ports {
	int in_ch;    /* declared input width  (4 per input slot)  */
	int out_ch;   /* declared output width (4 per output slot) */
};

/* Decode the port table from a config-announce CONTROL BLOCK (the 32 bytes at
 * frame[18:50]). Returns 0 and fills `out` when the block is a complete link-1
 * message whose OPCODE at block[4] is one of the three declaration arms, and
 * every table slot is a known code; -1 otherwise, leaving `out` untouched. */
int reac_ports_parse(const uint8_t block[32], struct reac_box_ports *out);

/* The head-amp base a desk grants for a declared input width. A head-amp
 * record's CH is base + (box_input - 1); every desk generation grants the same
 * base for the same declaration (42 grant sweeps across 82 captures,
 * reac-captures docs/PLACEMENT-EVIDENCE.md):
 *
 *     8 -> 0x00 (S-0808)    16 -> 0x20 (S-1608)    32 -> 0x00 (S-4000S)
 *
 * Returns -1 for a width with no captured placement — REFUSE, never guess:
 * base 0 for a 16-input box addresses its preamps 32 slots low. */
int reac_headamp_base(int in_ch);

#endif /* REAC_PORTS_H */
