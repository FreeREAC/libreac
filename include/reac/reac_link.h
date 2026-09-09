// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_link — THE REAC CONTROL PLANE, and the one header a daemon needs for it.
 *
 * Operator ruling, 2026-09-09: "move the FSM into libreac so the daemon is sockets +
 * PipeWire only. reac-pw cannot do reac ctrl protocol." Everything a REAC endpoint decides
 * — which end of a pairing a wire calls for, how a box enrols with a master, how a master
 * grants a box, when a link is held and when it drops — lives here, and it is PURE: no
 * socket, no thread, no clock of its own. A caller feeds it frames and ticks and is told
 * what to send.
 *
 * The pieces are the headers this one pulls in, each already documented where it lives:
 *   <reac/reac_hunt.h>, <reac/reac_arbitration.h>   which end of the pairing, from sightings
 *   <reac/reac_fsm.h>                                the slave's JOIN/HOLD table
 *   <reac/reac_master.h>, <reac/reac_master_fsm.h>   the master's establishment and grant
 *   <reac/reac_grant.h>, <reac/reac_headamp_tx.h>    the enrolment sweep and the preamp send
 *   <reac/reac_ctrl.h>, <reac/reac_ctrlblk.h>        the frames themselves
 *   <reac/reac_disco.h>, <reac/reac_link_state.h>    what is on a wire, and how it reads
 *
 * WHY IT IS ONE LIBRARY WITH TWO HEADERS. Control and audio share the FRAME — the block at
 * [16:50] and the braid at [50:1490] are two regions of one buffer — and they share the box
 * matrix, which the encoder reads for widths and the enrolment reads for declarations.
 * Nothing today links one without the other, and a second soname is a second version to
 * drift. This header names no encoder type, so a split stays a build change rather than a
 * redesign. See docs/REAC-CONTROL-PLANE.md.
 *
 * WHAT THE 2026-09-09 CAPTURES PROVED, and what a caller must therefore do, in order:
 *
 *   1. LISTEN before speaking, about two announce cadences. Both boxes that were granted had
 *      been silent for seconds first, and the flood is a decision that needs evidence.
 *   2. A master that ANNOUNCES ITSELF (`cfea`, ~1/s) is not hunted: no flood. A SILENT one
 *      is — that is what the 0.68 s bounded flood is for.
 *   3. Wait for the master's SCENE TRANSFER to go quiet (~200 ms), then send the
 *      config-announce declaring YOUR OWN inventory at your width
 *      (`reac_ctrl_build_config_announce_box_master`).
 *   4. About 200 ms later, the cold-connect burst — and then the HEARTBEAT on the very next
 *      frame, before any grant. Both granted boxes did exactly that
 *      (`16.1162` JOIN, `16.1164` BOX_READY, `16.1165` heartbeat, grant 2 ms later).
 *   5. The FILLERS in between carry the descriptor: zero, then REQUESTING, then ESTABLISHED
 *      once granted. A capture with that window zeroed is REFUSED by a real S-1608.
 *   6. Ask once and wait; retry only after a couple of seconds with no echo, never
 *      re-flooding a master that is already answering.
 */
#ifndef REAC_LINK_H
#define REAC_LINK_H

#include <reac/reac_ctrl.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_fsm.h>
#include <reac/reac_hunt.h>
#include <reac/reac_arbitration.h>
#include <reac/reac_master.h>
#include <reac/reac_disco.h>
#include <reac/reac_link_state.h>

/* THE DESCRIPTOR A FILLER'S CONTROL AREA CARRIES. Read off the wire 2026-09-09 and confirmed
 * by replay: the granted S-0808 sent 48 zero frames, then 0x52 for the 8691 frames between
 * its announce and the grant, then 0x7a for the rest. `spec/reac.ksy` has no name for it;
 * REQUESTING is what the wire shows it to mean. */
enum reac_link_desc {
	REAC_LINK_DESC_NONE        = 0x00,   /* before we have asked                    */
	REAC_LINK_DESC_REQUESTING  = 0x52,   /* announce sent, grant not yet received   */
	REAC_LINK_DESC_ESTABLISHED = 0x7a,   /* granted                                 */
};

/* Stamp `d` across the 32-byte control area of a FILLER, or clear it. */
void reac_link_fill_descriptor(uint8_t *frame, enum reac_link_desc d);

/* Which descriptor a slave owes, from its own state and whether it has announced yet. */
enum reac_link_desc reac_link_desc_for(enum reac_fsm_state st, int announced);

/* THE SLAVE'S JOIN BURST, one frame per step, in the order both granted boxes sent them:
 *
 *   step 0   cdea 04 03  TAG 0100  JOIN
 *   step 1   cdea 04 03  TAG 0302  BOX_READY
 *   step 2   cdea 01 03 0001 81    the heartbeat, on the very next frame
 *
 * TWO RECORDS, NOT THREE. `spec/reac.ksy` reads "tags 0100 / 0000 / 0302", and the second
 * ground truth shows why that is the GRANT rather than the join: the S-0808 sent only 0100
 * and 0302, and the S-1608 master answered echo(0100) + ITS OWN 0000 head_mark + echo(0302).
 * A slave that sends the master's record is sending back a line that is not its own;
 * `reac_ctrl_build_coldconnect_head` exists for the master side and for replay fixtures.
 *
 * Returns the frame length, or 0 past the last step. */
int reac_link_slave_burst(int step, uint8_t *out, const uint8_t master[6],
                          const uint8_t src[6], uint16_t counter, int n_ch,
                          float *const *planar, int ns);

#endif /* REAC_LINK_H */
