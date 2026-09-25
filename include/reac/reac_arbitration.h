/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * reac_arbitration — WHO DRIVES THIS SEGMENT, as a published fact.
 *
 * REAC keeps ONE master per segment, and until now nothing anywhere modelled that as a
 * first-class thing. Plugging an S-4000S in slave mode (2026-08-20) produced a sighting of
 * role `master`, a model that never resolved, and a reac-pw that probed forever, because the
 * segment's master topology existed only as scattered evidence nobody added up.
 *
 * This is increment 2 of `docs/design/specs/2026-08-20-reac-master-arbitration.md`: compute the
 * segment aggregate from the sightings the discovery table already holds plus our own FSM
 * state, and publish it. **PASSIVE — it decides nothing.** Joining a foreign master (§2) and
 * promoting the clock with none (§3) are later increments; this one exists so both can be built
 * against a fact rather than re-derived, and so an operator can SEE the topology that is
 * currently only inferable from a log.
 *
 * ARBITRATION KEYS ONLY ON UNAMBIGUOUS MASTER EVIDENCE. The catch-all sighting buckets never
 * feed it: the S-4000S incident IS a box misfiled through a catch-all, and the cost of acting
 * on that misfile is a dead segment. A sighting of UNKNOWN role stays a sighting — it is
 * neither promoted to a master nor counted as its absence.
 *
 * THE REAC CLOCK IS NOT THE PIPEWIRE CLOCK. `pace_source` names who owns the WIRE's
 * transmission pace and nothing else. The graph keeps its own clock whoever masters the
 * segment; reac-pw is the boundary and rate-matches across it. No arbitration outcome ever
 * re-parents the graph clock.
 */
#ifndef REAC_ARBITRATION_H
#define REAC_ARBITRATION_H

#include <reac/reac_disco.h>
#include <reac/reac_master.h>   /* enum reac_master_state: the FSM's OWN state */
#include <reac/reac_clock.h>    /* the discipline this vocabulary reports on */

#include <stdint.h>

/**
 * Who drives this segment.
 *
 * Distinct from `enum reac_master_state`, which is our FSM's own progress
 * (IDLE/PROBING/GRANTING/ESTABLISHED). That says what WE are doing; this says who OWNS the
 * wire. Both are needed and conflating them is how "we are probing" came to be read as "there
 * is no other master".
 */
enum reac_segment_master {
	/** The wire holds no master evidence at all. */
	REAC_SEGMENT_NONE = 0,
	/** Our FSM is established, or probing unopposed. */
	REAC_SEGMENT_US,
	/** An unambiguous OTHER master lives on this segment. */
	REAC_SEGMENT_FOREIGN,
};

/** Who owns the REAC pace — the speed of transmission on the wire — right now. */
enum reac_pace_source {
	/** We time the stream off CLOCK_MONOTONIC, disciplined by nothing. */
	REAC_PACE_FREE_RUN = 0,
	/** A foreign master times the stream and we recover its pace. */
	REAC_PACE_FOREIGN_MASTER,
	/** We time it, disciplined to a NIC/external PHC. */
	REAC_PACE_PHC,
	/** We time it, disciplined to a hardware-driven PipeWire graph clock as a FREQUENCY
	 *  reference. Never means the two domains merged. */
	REAC_PACE_GRAPH_REF,
	/** We time it, disciplined to the box's own counter slope. */
	REAC_PACE_BOX_SLOPE,
};

/** What a rival master turned out to be. The strings are the published prop values. */
enum reac_rival_kind {
	REAC_RIVAL_NONE = 0,
	/** The 40-channel downstream — a real desk. Joinable per §2. */
	REAC_RIVAL_DESK,
	/** A box width from something claiming master: a stagebox in the wrong mode. REFUSE. */
	REAC_RIVAL_BOX,
	/** No legal geometry heard yet. Refused too — §4's catch-all conservatism: a frame kind
	 *  nobody has captured must not flip the segment's topology. */
	REAC_RIVAL_UNKNOWN,
};

/** The segment aggregate, as the props carry it. */
struct reac_arbitration {
	enum reac_segment_master state;
	/** The driving master's MAC; meaningful only when {@link have_mac} is 1. */
	uint8_t mac[6];
	int have_mac;
	/** Who owns the wire pace. */
	enum reac_pace_source pace;
	/**
	 * A foreign master is live WHILE WE ARE ESTABLISHED — the mid-flight conflict.
	 *
	 * Reported rather than acted on: yielding drops a box mid-audio, holding breaks the
	 * one-master law, and which of those is right is the operator's call (spec §6 Q1). Until
	 * it is answered the rule is hold and report LOUDLY, and this flag is that report. It is
	 * deliberately separate from {@link state}, which stays `us` because we are in fact still
	 * driving — a surface that showed `foreign` here would say the desk had taken over when
	 * it has not.
	 */
	int conflict;
	/**
	 * WHAT THE RIVAL IS, decided by its frame GEOMETRY rather than by its control frames
	 * (spec §2b). A desk drives with the 40-channel downstream; a stagebox emits its own,
	 * smaller declared width — and a stagebox strapped to master mode claims master while
	 * emitting a box geometry. The two want opposite responses, so they cannot share a name.
	 * REAC_RIVAL_NONE when there is no rival at all.
	 */
	enum reac_rival_kind rival;
	/**
	 * THE WIDTH THE CLASSIFICATION WAS MADE FROM, in channels; 0 when the rival's frames
	 * carried no legal `52 + n*36` geometry (which is what makes {@link rival} UNKNOWN).
	 *
	 * The kind is a verdict and this is the evidence under it, and the difference is
	 * load-bearing since the 2026-09-09 ruling: a box that masters an unpinned wire is
	 * JOINED, and the segment's nodes are then sized from what that box announces. A
	 * caller that had only `box` would have to re-derive the width from the table — the
	 * classification and the number it was made from must not be recovered separately.
	 */
	unsigned rival_channels;
};


/**
 * Classify a rival by what it SAID and how it spoke — never by its width.
 *
 * Operator ruling 2026-09-25: "BOX_MAX_CHANNELS = 40 ... boxes have their size of ins and outs,
 * always even." A box may be 40 wide, so a 40-wide stream no longer proves a desk and a narrower
 * one no longer proves a box. The evidence, in order:
 *   - the peer DECLARED a box model (its own config-announce matched a row) -> BOX. A box
 *     strapped to master still declares what it is; that is the stagebox §2b refuses;
 *   - its role is BOX (a box-only signature: JOIN/box-ready/identity record, box heartbeat,
 *     config announce, or a UNICAST FILLER — a desk BROADCASTS its downstream) -> BOX;
 *   - no audio stream heard from it yet (channels 0) -> UNKNOWN, refused per §4;
 *   - it announced itself MASTER (cfea, slot map, head-amp records, scene push) and declared
 *     no box -> DESK.
 * NULL is NONE.
 */
enum reac_rival_kind reac_rival_kind_of(const struct reac_disco_entry *e);

/**
 * HOW LONG A BROADCAST SENDER IS GIVEN TO PROVE IT IS THE DESK.
 *
 * A broadcast audio stream whose source has sent nothing that names its role could be the
 * desk's downstream (its master-only ops not heard yet) or a box's presence-flood. Width
 * cannot decide it (a box may be 40 wide). Operator ruling 2026-09-25: HOLD WITH A DECLARED
 * LIMIT — hold the verdict until the sender's own frames prove its role, but never longer
 * than the SHORTEST master-only control cadence, in FRAMES at the current rate; at that
 * window's end a broadcast sender that sent no master-only op is a BOX. The desk reveals
 * itself fast: its cfea announce comes every 4000 frames of a 48 kHz downstream whether
 * or not a box answers (the page 0x0019 window slows and the scene transfer repeats at
 * longer counts). The window is a FRAME count per rate; a duration is only those frames
 * at a pace (operator ruling 2026-09-25: "ms depends on frequency and is a derived
 * figure").
 *
 * The cadence is a protocol fact: reac-protocol's master_cadence group
 * (MASTER_ONLY_CADENCE_FRAMES_44K1 / _48K / _96K = 3675 / 4000 / 8000, from the
 * m200i-s1608 cold-boot capture), read through the generated reac_facts_master_cadence.h
 * — never typed here.
 */
#include <reac/reac_facts_master_cadence.h>

/** The window in FRAMES at `fps` frames/s (snapped to the pace it means), or 0 when
 *  `fps` <= 0 (no rate known). */
uint32_t reac_master_only_cadence_frames(int fps);

/** The same window as a duration: its frames at `fps`. With no rate known (`fps` <= 0)
 *  it is the LONGEST of the three paces' windows — a hold that would be too short at some
 *  pace is not a hold. (The three are each rate's own frame count, not one duration: the
 *  44.1 and 96 kHz counts are INFERRED from the measured 48 kHz one by the per-rate law,
 *  and the three durations agree only through that law.) */
uint64_t reac_master_only_cadence_ns(int fps);

/**
 * What a sender IS, with the hold: reac_rival_kind_of() plus the window.
 *
 *   - declared a box model, or spoke as only a box does          -> BOX
 *   - announced master (a master-only op) and declared no box    -> DESK (UNKNOWN until a
 *     stream is heard, as in reac_rival_kind_of)
 *   - neither (a broadcast stream, role unresolved), no stream heard            -> UNKNOWN
 *   - neither, heard for less than one master-only cadence at `fps`             -> UNKNOWN
 *     (HOLD: its master-only op may still come)
 *   - neither, heard for the whole cadence with no master-only op               -> BOX
 *
 * `now_ns` is on the table's clock (the one reac_disco_table_observe was given); `fps` is
 * the sender's pace if known, else 0 (reac_master_only_cadence_ns). NULL is NONE. Pure.
 */
enum reac_rival_kind reac_sender_kind(const struct reac_disco_entry *e, uint64_t now_ns,
                                      int fps);

/**
 * WIDTH ONLY — SUPERSEDED for desk-vs-box by reac_rival_kind_of().
 *
 * It says 40 -> DESK and narrower -> BOX, which the 2026-09-25 ruling overturned: a box may be
 * 40 wide. Kept because it is public; nothing in libreac or libreac-transport decides
 * desk-vs-box with it any more (reac_segment_ident's width-only answers, kept for reac-pw
 * until it moves to the _kind variants, are the last callers). 0 is UNKNOWN.
 */
enum reac_rival_kind reac_rival_kind_from_channels(unsigned channels);

/** Wire name for a rival kind — `none` | `desk` | `box` | `unknown`. */
const char *reac_rival_kind_name(enum reac_rival_kind k);

/** The refusal code for a rival kind, or `"none"` when nothing is refused (no rival, or a
 *  desk, which is JOINED rather than refused). */
const char *reac_rival_refusal(enum reac_rival_kind k);

/**
 * The segment's coded refusal — why this segment is not carrying audio, in one word the
 * surface can render a remedy for.
 *
 * A rival always wins the report: a stagebox strapped to master is why nothing else can
 * happen. Otherwise the case auto-spine §3b names — a box HEARD but not joining, which a REAC
 * box does only on link-up, so it will sit there forever and no amount of waiting fixes it.
 * That state was a journal line every ten seconds and an eternal spinner on the surface; a
 * control refuses VISIBLY or it has not refused.
 *
 * `probing` is the master FSM's own answer about itself, `box_present` whether any box frame
 * has been classified on this wire, `joins` how many validated JOINs arrived. Pure.
 */
const char *reac_segment_refusal(enum reac_rival_kind rival, int probing,
                                 int box_present, uint64_t joins);

/** Wire names, stable across versions — these strings ARE the published prop values. */
const char *reac_segment_master_name(enum reac_segment_master s);
const char *reac_pace_source_name(enum reac_pace_source p);

/**
 * The pacer's clock discipline, in the vocabulary a segment PUBLISHES (0.5.4).
 *
 * Two enums describe one fact from opposite ends: `reac_clock_source` is what the DLL is
 * steering to, `reac_pace_source` is what a console reads off the segment's row. Nothing
 * mapped between them until 0.5.4, so the playback door published the constant
 * `REAC_PACE_FREE_RUN` — true under the 2026-08-21 config of record, in which clock-follow
 * was off, and false since 0.5.0 made following the default. Measured on the rig
 * 2026-09-08/09: the journal said `locked to graph clock (api.alsa.0)` and the prop said
 * `free-run`, which is the daemon contradicting itself in public.
 *
 * ONLY `REAC_CLOCK_LOCKED` NAMES A REFERENCE. LOCKING is a claim about the future and
 * HOLDOVER is a frozen period nothing is steering right now; both are running on
 * CLOCK_MONOTONIC at this instant, which is what free-run MEANS. Naming the device we
 * stopped following would be the same lie as a soft meter.
 *
 * The WIRE never reaches here in practice — a slave runs no pacer, and a segment paced by
 * a foreign master is told so by the arbitration itself — and is mapped anyway rather than
 * falling through as a reference we discipline ourselves to.
 *
 * PURE.
 */
enum reac_pace_source reac_pace_from_clock(enum reac_clock_source src,
                                           enum reac_clock_state state);

/**
 * Compute the segment aggregate.
 *
 * `fsm` is our own FSM's answer about itself, taken as its existing enum rather than as
 * booleans so there is one vocabulary for it. `now_ns` ages the sightings: a master heard once
 * and gone is not a master, and the table's own staleness bar is what decides that, so a
 * segment whose desk was unplugged stops reporting it.
 *
 * Pure: no clock of its own, no I/O, no state kept between calls.
 */
void reac_arbitrate(const struct reac_disco_table *table,
                    const uint8_t our_mac[6],
                    enum reac_master_state fsm,
                    enum reac_pace_source own_pace,
                    uint64_t now_ns,
                    struct reac_arbitration *out);

#endif /* REAC_ARBITRATION_H */
