// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The scene body reac-pw pushes.
 *
 * DATA, NOT PROTOCOL, which is why it stays in the daemon while the transfer that
 * carries it lives in libreac. The framing is the same for everyone; the contents
 * are one desk's state.
 *
 * CAPTURE-DERIVED: recovered from a real M-200i with tools/recover-scene.py (see
 * data/PROVENANCE.md). Our own MAC is substituted at REAC_SCENE_MAC_OFF before it
 * goes on the wire; nothing else is changed. It is a working example, not the
 * right end state — the box reads far more of this body than the three tags it
 * validates, so it also encodes an M-200i's idea of what the desk is. A console
 * that builds its own scene is the correct behaviour, and replacing this is
 * exactly that work.
 *
 * ONE BYTE IS NOT THE M-200i's: `revision` (u2le at +0x14) is 0x0001 here, not the
 * 0x0000 this body was recovered with. reac.ksy: `revision` carries the pace code,
 * the same value as cfea[19] — 0 at 48 kHz, 1 at 96 kHz, 2 at 44.1 kHz — and the box
 * CACHES it and compares before it will re-read the scene's three sub-objects — a
 * body whose revision differs is treated as changed with no further comparison
 * (evidenced in the firmware image and across the corpus). We announce pace code 1
 * (96 kHz) in cfea[19]; sending a scene revision of 0 underneath that is a
 * contradiction the box can see. The other two bytes that differ from an M-5000's
 * body (+0x366..367 and +0x22c6..7) are deliberately NOT copied: the ksy records
 * them as M-5000-only UNINITIALISED padding that changes between that desk's own
 * runs, so they carry no meaning to reproduce. */
#ifndef REAC_SCENE_BODY_H
#define REAC_SCENE_BODY_H

#include <reac/reac_ctrlblk.h>   /* REAC_SCENE_BYTES */

extern const uint8_t reac_scene_placeholder[REAC_SCENE_BYTES];

#endif /* REAC_SCENE_BODY_H */
