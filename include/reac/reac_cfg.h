// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_cfg — the `reac.cfg.*` / `reac.rate.*` PipeWire property vocabulary: how
 * a console reconfigures a RUNNING reac-pw over the graph instead of a config
 * file or a restart (docs/design/specs/2026-08-26-reac-runtime-config.md).
 *
 * ONE DECLARATION, BOTH SIDES (spec §2). reac-pw's `param_changed` reads these
 * keys off a segment's `reac-playback[.<inst>]` node and answers on them;
 * openmixer's TS mirror (`@openmixer/core`'s `reac-cfg.ts`) is conformance-
 * pinned against this file byte-for-byte, so a knob cannot exist as a string
 * literal in one repo and silently drift from the other.
 *
 * THE CHANNEL IS PROPS, NOT A FILE. A config file holds STARTUP values and
 * fixed site facts, read once and never watched; every LIVE reconfiguration —
 * rate, role — travels as an `SPA_PROP_params` entry on the node, the same
 * mechanism `reac.headamp.<ch>.<param>` already proves live: the console
 * writes, `param_changed` applies and answers in props, and a re-clock is
 * absorbed by the daemon RE-ESTABLISHING itself rather than a restart.
 *
 * RATE IS OPERATOR-ONLY. The daemon never picks a rate on its own initiative.
 * With no operator assertion standing it drives the HIGHEST rate it can
 * actually pace — never a guess, and never the closed list's first member.
 *
 * THE PACE LIST IS CLOSED AND DECLARED; DRIVABILITY IS OBSERVED. REAC has
 * three rates by definition (the three below) and that list never changes at
 * runtime. What a given NIC / clock source / host can actually pace is a
 * MEASUREMENT the daemon publishes beside its chosen default
 * (`reac.rate.drivable`) — never assumed from the closed list alone.
 *
 * REFUSED, NEVER CLAMPED. An asserted rate outside the drivable subset, off
 * the closed list, or asserted onto a segment with no rate to own (a slave)
 * comes back as a refusal CODE on `reac.cfg.rate.refused` — one of the three
 * short kebab strings below — never a silently substituted value.
 */
#ifndef REAC_CFG_H
#define REAC_CFG_H

/* ---- the config channel: what the console WRITES -------------------------- */

/* The desired REAC pace, Hz, as a decimal ASCII SPA prop. Legal values are
 * exactly the three below; anything else is refused (`not-in-list`) before it
 * ever reaches the pacer. Written on the segment's own
 * `reac-playback[.<inst>]` node — the same node `reac.headamp.*` already
 * addresses, through the same door. */
#define REAC_CFG_RATE_PROP            "reac.cfg.rate"

/* The console's own desired ROLE for this segment — decimal ASCII 0 (master)
 * or 1 (slave), the same numeric-flag convention `reac.headamp.<ch>.phantom`
 * already uses for a two-state fact travelling as an SPA Props value.
 * AUTODETECT IS FOR BOXES; our own role is CHOSEN, never sensed, so there is
 * no separate "observed" counterpart key for it — reading this prop back IS
 * reading the daemon's own answer (spec §1: "the daemon's `param_changed`
 * applies them and ANSWERS in props"). */
#define REAC_CFG_ROLE_PROP            "reac.cfg.role"
#define REAC_CFG_ROLE_MASTER          0
#define REAC_CFG_ROLE_SLAVE           1

/* ---- the config channel: how the daemon ANSWERS ---------------------------- */

/* A refusal code for the last `reac.cfg.rate` write, or absent/empty when the
 * last write landed (or none was ever refused). Never silent: a rate outside
 * the drivable subset, off the closed list, or asserted onto a segment with
 * no rate to own all land here, never as a silent clamp. One of the three
 * REAC_CFG_REFUSED_* strings below. */
#define REAC_CFG_RATE_REFUSED_PROP    "reac.cfg.rate.refused"

/* ---- the observed facts: what the daemon PUBLISHES ------------------------- */

/* The segment's ACTUAL running pace, Hz, decimal ASCII — the measured fact,
 * independent of whether it came from an operator assertion or the daemon's
 * own best-drivable default. */
#define REAC_RATE_PROP                "reac.rate"

/* The segment's drivable subset of the closed pace list — what THIS NIC,
 * clock source and machine can actually pace, comma-separated decimal ASCII
 * Hz values low to high (e.g. "44100,48000"). A rate outside this set is
 * refused on `reac.cfg.rate.refused`, never accepted and then silently short. */
#define REAC_RATE_DRIVABLE_PROP       "reac.rate.drivable"

/* Why the segment runs at `reac.rate` right now — one of the two strings
 * below. */
#define REAC_RATE_SOURCE_PROP         "reac.rate.source"
#define REAC_RATE_SOURCE_ASSERTED     "asserted"  /* an operator's own write stands */
#define REAC_RATE_SOURCE_DEFAULT      "default"   /* no assertion — best drivable   */

/* ---- the CLOSED pace list: REAC has three rates by definition -------------- */

#define REAC_CFG_RATE_44100  44100
#define REAC_CFG_RATE_48000  48000
#define REAC_CFG_RATE_96000  96000

/* The count of members in the closed list, for a caller that wants to assert
 * it rather than count braces. */
#define REAC_CFG_RATE_COUNT  3

/* The list itself, for a caller that wants to iterate it rather than name each
 * member — e.g. the drivability probe's "which of these three can this NIC
 * pace" sweep. A brace-init, not a scalar prop — nothing on the wire carries
 * this shape directly. */
#define REAC_CFG_RATE_LIST_INIT { REAC_CFG_RATE_44100, REAC_CFG_RATE_48000, REAC_CFG_RATE_96000 }

/* ---- refusal codes: why `reac.cfg.rate` was refused, on `.refused` -------- */

/* The written value is not one of the three closed-list rates at all. */
#define REAC_CFG_REFUSED_NOT_CLOSED         "not_closed"

/* The value IS one of the three, but this segment's NIC/clock cannot pace it
 * (outside `reac.rate.drivable`). */
#define REAC_CFG_REFUSED_NOT_DRIVABLE       "not_drivable"

/* This segment is a SLAVE: its pace follows a foreign master physically, and a
 * slave owns no rate to set. */
#define REAC_CFG_REFUSED_ROLE_SLAVE         "role_slave"
/* The assertion did not parse as a rate at all — a malformed pod, not a wrong value. */
#define REAC_CFG_REFUSED_MALFORMED          "malformed"
/* Empty string: nothing refused — the standing answer when the last assertion applied. */
#define REAC_CFG_REFUSED_NONE               ""

/* The assertion's lifecycle, published beside the refusal: an accepted rate re-establishes
 * the segment, so the truth passes through "pending" before it is "applied" and every
 * surface can show the swap instead of guessing at it. */
#define REAC_CFG_RATE_STATE_PROP            "reac.cfg.rate.state"
#define REAC_CFG_RATE_STATE_PENDING         "pending"
#define REAC_CFG_RATE_STATE_APPLIED         "applied"

#endif /* REAC_CFG_H */
