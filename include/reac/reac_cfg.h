// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_cfg — the `reac.cfg.*` / `reac.rate.*` PipeWire property vocabulary: how
 * a console reconfigures a RUNNING reac-pw over the graph instead of a config
 * file or a restart (2026-08-26-reac-runtime-config).
 *
 * ONE DECLARATION, BOTH SIDES (spec §2). reac-pw's `param_changed` reads these
 * keys off a segment's `reac-playback[.<inst>]` node and answers on them;
 * a console that mirrors these names is expected to pin them against this file
 * byte-for-byte, so a knob cannot exist as a string literal in two places and
 * silently drift.
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
 * comes back as a refusal CODE on `reac.cfg.rate.refused` — one of the
 * REAC_CFG_REFUSED_* codes below — never a silently substituted value.
 *
 * THIS FILE IS THE DECLARATION, AND THE ONLY ONE. reac-pw's reac_rate_cfg.h and
 * reac_role_cfg.h include it and name these macros; they spell none of the strings
 * themselves. The refusal enums, the drivable-rate masks and the role answers that
 * libreac-transport reads (reac_pacer.h, reac_role_swap.h) are declared here too, so
 * libreac builds without a reac-pw header. A key or code typed a second time
 * anywhere is a red test (tests/conformance-declared-once.sh).
 */
#ifndef REAC_CFG_H
#define REAC_CFG_H

/* ---- the config channel: what the console WRITES -------------------------- */

/* The desired REAC pace, Hz, as a decimal ASCII SPA prop. Legal values are
 * exactly the three below; anything else is refused (`not_closed`) before it
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

/* The RUNNING role, published: "0" | "1", the same encoding as the write side. */
#define REAC_ROLE_PROP                "reac.role"

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
/* Operator, 2026-08-26: "default is not a valid value — we make the best the default, it
 * is a convention." No assertion standing means the daemon runs the best drivable rate BY
 * CONVENTION; any rate the operator gave (a flag, a conf file, a console assertion over
 * the graph) is ASSERTED. */
#define REAC_RATE_SOURCE_CONVENTION   "convention" /* no assertion — best drivable   */

/* ---- the CLOSED pace list: REAC has three rates by definition -------------- */

#define REAC_CFG_RATE_44100  44100
#define REAC_CFG_RATE_48000  48000
#define REAC_CFG_RATE_96000  96000

/* One bit per closed-list rate: the DRIVABLE MASK a segment publishes and the pacer
 * keeps. REAC_RATE_ALL_BITS is the honest "assume everything not yet measured". */
#define REAC_RATE_BIT_44100  (1u << 0)
#define REAC_RATE_BIT_48000  (1u << 1)
#define REAC_RATE_BIT_96000  (1u << 2)
#define REAC_RATE_ALL_BITS   (REAC_RATE_BIT_44100 | REAC_RATE_BIT_48000 | REAC_RATE_BIT_96000)


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
/* Nothing refused — the standing answer when the last assertion applied. "none" is
 * the estate's sentinel for "no value applies" (reac.master.mac, and libreac's own
 * reac_rival_refusal()); it was "" here while reac-pw published "none", so the two
 * sides of one prop disagreed on its idle value. */
#define REAC_CFG_REFUSED_NONE               "none"

/* Why a `reac.cfg.rate` assertion was refused. REAC_RATE_REFUSE_NONE doubles as the
 * published state once a refusal is superseded by an accepted rate. */
enum reac_rate_refuse {
	REAC_RATE_REFUSE_NONE = 0,
	REAC_RATE_REFUSE_NOT_CLOSED,    /* not one of 44100 / 48000 / 96000           */
	REAC_RATE_REFUSE_NOT_DRIVABLE,  /* in the closed list, outside this segment's */
	REAC_RATE_REFUSE_ROLE_SLAVE,    /* a slave has no rate setting of its own     */
	REAC_RATE_REFUSE_MALFORMED,     /* the prop value was not a usable number     */
};

/* The published code for each rate refusal, indexed by enum reac_rate_refuse: the table a
 * consumer's refuse-code function returns from, so no code is typed twice. */
#define REAC_RATE_REFUSE_CODES_INIT { \
	[REAC_RATE_REFUSE_NONE]         = REAC_CFG_REFUSED_NONE,         \
	[REAC_RATE_REFUSE_NOT_CLOSED]   = REAC_CFG_REFUSED_NOT_CLOSED,   \
	[REAC_RATE_REFUSE_NOT_DRIVABLE] = REAC_CFG_REFUSED_NOT_DRIVABLE, \
	[REAC_RATE_REFUSE_ROLE_SLAVE]   = REAC_CFG_REFUSED_ROLE_SLAVE,   \
	[REAC_RATE_REFUSE_MALFORMED]    = REAC_CFG_REFUSED_MALFORMED,    \
}

/* Why a `reac.cfg.role` assertion was refused. REAC_ROLE_REFUSE_NONE doubles as the
 * published state once a refusal is superseded by an accepted role. Role has no
 * drivability-style constraint the way rate does, so there is one refusal today. */
enum reac_role_refuse {
	REAC_ROLE_REFUSE_NONE = 0,
	REAC_ROLE_REFUSE_MALFORMED,   /* the prop value was not 0 or 1 */
};

/* The published code for each role refusal, indexed by enum reac_role_refuse. */
#define REAC_ROLE_REFUSE_CODES_INIT { \
	[REAC_ROLE_REFUSE_NONE]      = REAC_CFG_REFUSED_NONE,      \
	[REAC_ROLE_REFUSE_MALFORMED] = REAC_CFG_REFUSED_MALFORMED, \
}

/* The assertion's lifecycle, published beside the refusal: an accepted rate re-establishes
 * the segment, so the truth passes through "pending" before it is "applied" and every
 * surface can show the swap instead of guessing at it. */
#define REAC_CFG_RATE_STATE_PROP            "reac.cfg.rate.state"
#define REAC_CFG_RATE_STATE_PENDING         "pending"
#define REAC_CFG_RATE_STATE_APPLIED         "applied"

/* The role answer, published beside reac.cfg.role. A same-role assertion is applied
 * immediately; a role CHANGE (master<->slave) tears down one engine and builds the
 * other, which cannot complete without a live re-attach, so it is answered
 * "role_reestablish_pending" and never a fake "applied". */
#define REAC_CFG_ROLE_STATE_PROP            "reac.cfg.role.state"
#define REAC_CFG_ROLE_REFUSED_PROP          "reac.cfg.role.refused"
#define REAC_CFG_ROLE_STATE_APPLIED         "applied"
#define REAC_CFG_ROLE_STATE_PENDING         "role_reestablish_pending"
/* The names the role answer is read under in libreac-transport (reac_role_swap.c) and in
 * reac-pw: aliases of the two above, never a second spelling. */
#define REAC_ROLE_STATE_APPLIED             REAC_CFG_ROLE_STATE_APPLIED
#define REAC_ROLE_STATE_REESTABLISH_PENDING REAC_CFG_ROLE_STATE_PENDING
/* The swap landed on a slave that no master has enrolled yet: the role is right, the
 * job it names is not being done (reac_role_swap.h). */
#define REAC_CFG_ROLE_STATE_HUNTING         "role_hunting"
/* A TAP's answer: it serves what it hears and asserts nothing (REAC_ROLE_INTENT_TAP,
 * <reac/transport/reac_tap.h>). It never reads "applied", because it performs no role on
 * the wire, and never "role_hunting", because it courts no master. */
#define REAC_CFG_ROLE_STATE_TAP             "role_tap"

#endif /* REAC_CFG_H */
