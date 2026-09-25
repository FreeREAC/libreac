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
 * so a width is even and the narrowest is one pair. A frame that is
 * MAX_CHANNELS wide (1492 B) is the desk's downstream broadcast and only
 * that: reac.ksy classifies by width alone (`is_downstream_width` is
 * `num_channels == 40`), and its `num_channels` doc already reads "40 is
 * the downstream broadcast; an even 2..38 is a box's upstream return". A
 * 40-wide box frame would parse as the desk's. So the widest box is one
 * pair short of it.
 *
 * The operator's ruling of 2026-09-25 says the same: "mixer sends 40ch,
 * boxes have their size of ins and outs, always even." The widest box in
 * the corpus is 32 (BOX_S4000S_3208_IN, BOX_S4000S_0832_OUT), inside the
 * range. 34..38 are not observed; they are legal by this rule, not
 * measured.
 */
/* The narrowest box width in either direction, one braid pair. [derived — the
 * braid pairs channels (BRAID_PAIR_CHANNELS).]
 */
#define REAC_BOX_MIN_CHANNELS           2

/* The widest box width in either direction. A MAX_CHANNELS-wide frame is the
 * desk's downstream broadcast, so a box stops one pair short of it. [derived
 * — reac.ksy is_downstream_width (a 40-wide frame is the downstream
 * broadcast) and num_channels ("an even 2..38 is a box's upstream return");
 * operator ruling 2026-09-25. Widest box in the corpus is 32.]
 */
#define REAC_BOX_MAX_CHANNELS           38

#endif /* REAC_FACTS_BOX_WIDTH_H */
