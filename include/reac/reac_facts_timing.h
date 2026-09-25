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

#ifndef REAC_FACTS_TIMING_H
#define REAC_FACTS_TIMING_H

/* ---- Timing a second implementation has to match -------------------------------
 * Periods and budgets a box or a desk FIXES and a peer must honour, as
 * opposed to this project's own tunables (which live in libreac's
 * reac_tunables.h and are not protocol). Each was a literal in libreac's
 * reac_fsm.h / reac_master.h / reac_master.c.
 */
/* The box firmware's established link-check reload, in frames. [EVIDENCED
 * (image) — 0x0258 in the S-1608 image.]
 */
#define REAC_BOX_LINKCHECK_RELOAD_FRAMES 600   /* 0x0258 */

/* A master announces, and while established sends one chanmap window, once a
 * second. [EVIDENCED (corpus + rig) — at the hunt rate (~0.37/s) the box's
 * link light kept blinking (rig, 2026-07-12).]
 */
#define REAC_ANNOUNCE_PERIOD_MS         1000

/* A desk's scene push rate — 341 chunks in ~0.68 s. [EVIDENCED (corpus) —
 * M-200i -> S-1608, SCENE_CHUNKS evidence.]
 */
#define REAC_SCENE_BURST_CHUNKS_PER_SEC 500

/* One echoed grant per twelve frame slots across the ~150 ms grant burst.
 * [EVIDENCED (corpus) — the transcribed real burst.]
 */
#define REAC_GRANT_STRIDE_SLOTS         12

/* A desk's dwell between the enroll group map and the grant burst. [EVIDENCED
 * (corpus) — 1503 ms on matrix-m200-s0808 and 1717 ms on matrix-m200-s1608
 * (2026-07-11); nominal.]
 */
#define REAC_ENROLL_GRANT_DWELL_MS      1600

/* How long a desk rides through box silence before it reverts to hunting.
 * [EVIDENCED (rig) — one M-200i reboot measurement, 2026-07-11 (heartbeat
 * stops t=16.0 s, first probe t=22.47 s).]
 */
#define REAC_MASTER_LINK_HOLD_MS        6500

#endif /* REAC_FACTS_TIMING_H */
