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
 * real M-200i driving a real S-1608 at 48 kHz
 * (m200i-s1608-48k-mirror__real-m200-s1608-coldboot, 2026-07-11; the
 * enrolment timeline in reac.ksy and docs/mixer-protocol.md §3/§4/§7), in
 * frames of that 4000 fps downstream:
 *
 *   cfea master announce one every 4000 frames, steady AND with no
 *                              box answering
 *   op 01 03 page 0x0019 one every 4000 frames; slows to ~8000 while
 *                              no box answers
 *   the scene transfer op 01 01 header, 341 x op 01 00, op 01 02
 *                              final, the WHOLE transfer again every
 *                              ~10780 frames while the box stays silent
 *
 * The SHORTEST of these is the window a consumer holds a broadcast sender's
 * verdict for: a sender that has sent no master-only op for that many of its
 * own frames is not a desk. That is the cfea announce's, and it is the one
 * of the three that does not slow when no box answers. (The page 0x0019
 * slowdown and the scene repeat are approximate, so they are described here
 * and not declared as numbers.)
 *
 * FRAMES, PER RATE, ARE THE FACT (operator ruling, 2026-09-25: "we must only
 * consider the frames per rate — ms depends on frequency and is a derived
 * figure"). A desk counts its cadences in frames, so the window is a frame
 * count at the current rate and is declared as one; a duration is that
 * count over PKT_RATE and is never declared here. The 48 kHz count is
 * measured. The 44.1 and 96 kHz counts are INFERRED by the same per-rate law
 * (the 48 kHz count scaled by each rate's PKT_RATE) until a capture at that
 * rate measures them.
 */
/* The shortest cadence, in frames at 48 kHz, at which a desk sends a
 * master-only op (the cfea announce) — how many of a broadcast sender's own
 * frames may pass before its silence proves it is not a desk. [EVIDENCED
 * (corpus) — m200i-s1608-48k-mirror__real-m200-s1608-coldboot 2026-07-11, one
 * cfea announce every 4000 frames of the 48 kHz downstream, with and without
 * the box answering.]
 */
#define REAC_MASTER_ONLY_CADENCE_FRAMES_48K 4000

/* The master-only cadence in frames at 44.1 kHz. [INFERRED — the 48 kHz count
 * scaled by PKT_RATE_44K1 / PKT_RATE_48K; no 44.1 kHz capture measures it
 * yet.]
 */
#define REAC_MASTER_ONLY_CADENCE_FRAMES_44K1 3675

/* The master-only cadence in frames at 96 kHz. [INFERRED — the 48 kHz count
 * scaled by PKT_RATE_96K / PKT_RATE_48K; no 96 kHz capture measures it yet.]
 */
#define REAC_MASTER_ONLY_CADENCE_FRAMES_96K 8000

#endif /* REAC_FACTS_MASTER_CADENCE_H */
