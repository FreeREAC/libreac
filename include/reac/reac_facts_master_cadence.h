// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// GENERATED FILE - DO NOT EDIT.
//
// Source:    spec/protocol-facts.yaml   (in FreeREAC/reac-protocol)
// Generator: spec/gen-facts.py
//
// Edit the schema and regenerate. A hand-edit here is erased by the next run
// and, worse, is invisible to the cross-check that keeps reac.ksy and libreac
// agreeing - which is the whole reason this file is generated.

#ifndef REAC_FACTS_MASTER_CADENCE_H
#define REAC_FACTS_MASTER_CADENCE_H

/* ---- How soon a desk proves it is the desk -------------------------------------
 * A desk's downstream carries MASTER-ONLY control ops on a fixed cadence, so
 * a desk proves its role within one cadence of being heard. Measured on a
 * real M-200i driving a real S-1608
 * (m200i-s1608-48k-mirror__real-m200-s1608-coldboot, 2026-07-11; the
 * enrolment timeline in reac.ksy and docs/mixer-protocol.md §3/§4):
 *
 *   cfea master announce 1 Hz, steady AND with no box answering
 *   op 01 03 page 0x0019 1 Hz; slows to ~2 s while no box answers
 *   the scene transfer op 01 01 header, 341 x op 01 00, op 01 02
 *                              final, the WHOLE transfer again every
 *                              2.695 s while the box stays silent
 *
 * The SHORTEST of these is the window a consumer holds a broadcast sender's
 * verdict for: a sender that has sent no master-only op for that long is not
 * a desk. That is the cfea announce's period, ANNOUNCE_PERIOD_MS, and it is
 * the one of the three that does not slow when no box answers. (The page
 * 0x0019 slowdown is approximate, "~2 s", so it is described here and not
 * declared as a number.)
 *
 * IN FRAMES, AT THE CURRENT RATE. A desk counts its cadences in frames: one
 * announce per second is one per PKT_RATE frames, so the window is a frame
 * count that scales with the pace — 3675 / 4000 / 8000. The 48 kHz figure is
 * measured; the 44.1 and 96 kHz figures are the same one-per-second law at
 * those packet rates (the rate facts are measured, the per-second announce
 * at those rates is this group's derivation).
 */
/* While a box stays silent a desk repeats its whole scene transfer at this
 * period. [EVIDENCED (corpus) —
 * m200i-s1608-48k-mirror__real-m200-s1608-coldboot 2026-07-11, "the WHOLE
 * TRANSFER AGAIN every 2.695 s" (reac.ksy enrolment timeline;
 * docs/mixer-protocol.md §7).]
 */
#define REAC_SCENE_REPEAT_PERIOD_MS     2695

/* The shortest cadence at which a desk sends a master-only op — how long a
 * broadcast sender may be held before its silence proves it is not a desk.
 * [derived — the shorter of ANNOUNCE_PERIOD_MS (1 Hz cfea, which does not
 * slow with no box) and SCENE_REPEAT_PERIOD_MS.]
 */
#define REAC_MASTER_ONLY_CADENCE_MS     1000

/* MASTER_ONLY_CADENCE_MS in frames at 44.1 kHz. [derived — PKT_RATE_44K1
 * frames per second.]
 */
#define REAC_MASTER_ONLY_CADENCE_FRAMES_44K1 3675

/* MASTER_ONLY_CADENCE_MS in frames at 48 kHz. [derived — PKT_RATE_48K frames
 * per second; the 1 Hz cfea is measured at this rate (see the group doc).]
 */
#define REAC_MASTER_ONLY_CADENCE_FRAMES_48K 4000

/* MASTER_ONLY_CADENCE_MS in frames at 96 kHz. [derived — PKT_RATE_96K frames
 * per second.]
 */
#define REAC_MASTER_ONLY_CADENCE_FRAMES_96K 8000

#endif /* REAC_FACTS_MASTER_CADENCE_H */
