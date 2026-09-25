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

#ifndef REAC_FACTS_BOX_WIDTH_H
#define REAC_FACTS_BOX_WIDTH_H

/* ---- A box's width, either direction -------------------------------------------
 * What channel counts a BOX may carry, in either direction: an even
 * width from BOX_MIN_CHANNELS to BOX_MAX_CHANNELS. 0 in one direction
 * means the box has no ports that way and is not a width.
 *
 * Two facts already declared fix it, so both rows are derived and add no
 * new number. The braid carries channels in pairs (BRAID_PAIR_CHANNELS),
 * so a width is even and the narrowest is one pair. The widest is the
 * whole audio fabric, MAX_CHANNELS: a box may fill all 40 slots either
 * way, so a 40-wide (1492 B) frame is NOT only the desk's.
 *
 * WIDTH NEVER TELLS A DESK FROM A BOX. The operator's ruling of
 * 2026-09-25: "BOX_MAX_CHANNELS = 40. We are dealing with a S-4000S-3208
 * (32 in, 8 out), we also have S-2416 (24 in, 16 out), and we tested an
 * 8 in / 32 out box." Boxes have their own size of ins and outs, always
 * even, up to the fabric. What separates the desk's downstream from a
 * box's upstream is the frame's direction, source and role, not its
 * length; a box sending 40 upstream is a box. (reac.ksy's
 * `is_downstream_width` is a width predicate and nothing more — it says
 * a frame is 40 wide, not who sent it.)
 *
 * The widest box in the corpus is 32 (BOX_S4000S_3208_IN,
 * BOX_S4000S_0832_OUT). 34..40 are legal by this rule, not measured.
 */
/* The narrowest box width in either direction, one braid pair. [derived — the
 * braid pairs channels (BRAID_PAIR_CHANNELS).]
 */
#define REAC_BOX_MIN_CHANNELS           2

/* The widest box width in either direction — the whole audio fabric. A box
 * may be 40 wide either way; width is never what separates a box's frame from
 * the desk's. [derived — the audio fabric (MAX_CHANNELS); operator ruling
 * 2026-09-25 (BOX_MAX_CHANNELS = 40; S-4000S-3208 32/8, S-2416 24/16, and an
 * 8 in / 32 out box tested). Widest box in the corpus is 32.]
 */
#define REAC_BOX_MAX_CHANNELS           40

#endif /* REAC_FACTS_BOX_WIDTH_H */
