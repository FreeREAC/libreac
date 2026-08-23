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
	int headamp_base;  /* the box's own head-amp CH base, block[7] * 0x10 */
};

/* Decode the port table from a config-announce CONTROL BLOCK (the 32 bytes at
 * frame[18:50]). Returns 0 and fills `out` when the block is a complete link-1
 * message whose OPCODE at block[4] is one of the three declaration arms, and
 * every table slot is a known code; -1 otherwise, leaving `out` untouched. */
int reac_ports_parse(const uint8_t block[32], struct reac_box_ports *out);

/* ---- THE HEAD-AMP BASE IS ANNOUNCED, NOT GRANTED ------------------------
 *
 * A box's head-amp CH base is the box's OWN property. It rides the config
 * announce (`01 03 00 10`) at block[7] — a chassis strap the box reads once —
 * and the master addresses the box at base = block[7] * 0x10. A head-amp
 * record's CH is base + (box_input - 1).
 *
 * NOTHING IN THE BOX CONSUMES A GRANTED BASE. A master cannot move where a
 * head-amp write lands by granting differently: the fabric-row-to-preamp map is
 * a GPIO strap and a fitted-board inventory, both read before any frame
 * arrives. The decisive observation is that an 8-input and a 32-input box are
 * BOTH addressed at 0x00 — a width cannot be what selects the base.
 *
 * THIS REPLACES A PER-WIDTH TABLE, and the way that table survived is the
 * lesson. The retired mapping took an input count and returned 8 -> 0x00,
 * 16 -> 0x20, 32 -> 0x00. It agreed with the wire on every box we own, because
 * width and strap are collinear across the three chassis we have: a 16-input
 * chassis always straps 2. It was a second declaration of a fact the box
 * already states, correct only by coincidence, and wrong in a way nothing
 * downstream could detect. The first box that breaks the collinearity would
 * have had its preamps addressed 32 slots off with every gate still green.
 *
 * Evidence: S-1608.BIN (SH-4 LE, base 0x0BFE0000) FUN_0c003c8a at 0x0c003c8a
 * sets buf[7] = *0x0c080918, a GPIO strap read before the RTOS starts;
 * FUN_0c0081f6 applies it through FUN_0c007fbc(bank, group). Corroborated on 29
 * captures across M-200i, M-300 and M-5000 (reac-captures
 * analysis/placement_table.csv): S-0808 0x00->0x00, S-1608 0x02->0x20,
 * S-4000S 0x00->0x00.
 *
 * THE BASE IS A REQUIRED PROPERTY OF A BOX, not an optional one. It is set
 * here, once, when the announce parses, and there is deliberately no sentinel
 * for "not known yet" and no function that turns a width into a base. A box
 * that has not announced has not followed the grammar, so it is not an
 * incompletely-known box — it is not a box at all, and there is nothing to
 * address. Modelling that state would put a branch in every caller that can
 * never be taken. A caller holding a struct reac_box_ports has a base; a
 * caller that has no box has nothing to ask about. */

/* block-relative offset of the chassis strap the base is built from */
#define REAC_HEADAMP_BASE_OFF 7

/* base = block[REAC_HEADAMP_BASE_OFF] * this. Sixteen head-amp rows per strap
 * step, which is two REAC_HEADAMP_APPLY_UNIT_SLOTS groups. */
#define REAC_HEADAMP_BASE_MULTIPLIER 0x10

/* The law rows, spelled so protocol-facts.yaml can bind them and so a change
 * of mind has to edit a constant rather than drift a comment. */
#define REAC_HEADAMP_BASE_FROM_CONFIG_BYTE7    1
#define REAC_HEADAMP_BASE_IS_CHASSIS_NOT_GRANT 1

/* THE BOX APPLIES HEAD-AMP IN GROUPS OF EIGHT fabric rows, one 8-port board at
 * a time, passing the within-group index 0..7 to the preamp (S-1608.BIN
 * FUN_0c007fbc at 0x0c007fbc: slot = group << 3, eight iterations).
 *
 * THIS IS THE APPLY UNIT AND IT IS A DIFFERENT AXIS FROM ACTUATION. Actuation
 * is PER CHANNEL — 2304 of 3651 phantom records in the corpus address a
 * channel that is not a multiple of four, and writing phantom off to a group
 * neighbour of a live condenser left that mic ~45 dB above the floor. Neither
 * number is evidence about the other; do not fold them together. */
#define REAC_HEADAMP_APPLY_UNIT_SLOTS 8

#endif /* REAC_PORTS_H */
