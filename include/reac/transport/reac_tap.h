// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_tap — the PASSIVE segment role: listen, serve what is heard, transmit NOTHING.
 *
 * The third receiver beside reac_pacer (master: we drive the establishment and own the
 * clock) and reac_slave (slave: an external master drives it and we ANSWER). A tap does
 * neither. It opens no TX socket, sends no announce, asks for no grant, takes no
 * seglock, and holds no segment lock — there is no code path in this file that reaches
 * the wire outbound, and tools/conformance-tap-silent.sh asserts that mechanically with
 * reac_slave.c as its positive control.
 *
 * WHY A THIRD ROLE AND NOT "reac_slave WITH TX OFF". Because the two put different
 * things on the wire, and the difference was measured. Beside a real Roland desk on
 * 2026-09-12, a courting slave of ours kept the desk's own S-1608 from enrolling for
 * 180 s, and a GRANTED slave of ours blocked it outright while it rebooted — four
 * trials. A role that never transmits is a different role, not a quieter one
 * (openmixer docs/design/specs/2026-08-20-reac-master-arbitration.md, eighth amendment;
 * 2026-09-13-reac-plug-and-play.md §0/§4).
 *
 * WHAT IT SERVES. Per segment it hears: the master's 40-channel downstream broadcast,
 * and ONE upstream stream per box source MAC at that box's own width. Each stream gets
 * its own ring and its own reac_rx feeder, so the counter continuity, the duplicate
 * guard and the ppm slope of one stream can never be mixed into another's — that
 * separation is reac_rx's existing per-instance state, reused rather than re-derived.
 *
 * WHERE IT IS USEFUL. Only on a SWITCH MIRROR. An established box unicasts its return
 * to the desk, so plain membership of the segment shows a tap the desk's broadcast and
 * nothing of the stage. That is an admin requirement on the venue switch, not something
 * the console can arrange (plug-and-play §1, scenario B).
 *
 * THE MIRROR IS ALSO WHY THE DUPLICATE GUARD IS LOAD-BEARING HERE. A port mirrored in
 * BOTH directions delivers every transiting frame twice, one copy carrying 2 bytes of
 * the frame's own Ethernet FCS after the C2 EA marker and the other not (1492/1494
 * downstream, 628/630 upstream). Counted naively the second copy is not a gap — the
 * counter repeats rather than advancing — but the frame accounting a tap publishes has
 * to say so, and feeding both copies to a ring granulates the audio and doubles the
 * apparent rate. Both are handled on the CLEAN length, per stream, exactly as
 * reac_rx's guard does (see reac_rx.h's duplicate-guard note and its corpus evidence).
 *
 * THE RATE COMES FROM THE MASTER'S CADENCE, the same fact a slave locks to: pps
 * measured across the master's own (dup-collapsed) frames, snapped by reac_rate_snap.
 * A tap never asks a box what rate it runs at and never carries a default it did not
 * measure — reac_tap_survey_rate answers 0 when it heard no master, and 0 is an
 * absence, never 48000.
 */
#ifndef REAC_TAP_H
#define REAC_TAP_H

#include <stdint.h>
#include <stddef.h>

#include <reac/reac.h>
#include <reac/transport/reac_ring.h>
#include <reac/transport/reac_rx.h>

/* One master plus REAC_DISCO_MAX boxes — the same ceiling reac_disco's table carries,
 * for the same reason (RT discipline: fixed size, saturate rather than evict, and a
 * full roster is REPORTED as full, never passed off as a complete picture). */
#define REAC_TAP_MAX_STREAMS 9

enum reac_tap_stream_kind {
	REAC_TAP_STREAM_MASTER = 0,  /* the desk's downstream broadcast (proved by its announce) */
	REAC_TAP_STREAM_BOX,         /* one box's upstream return, at the box's width */
	/* A BROADCAST stream whose source has not proven what it is yet (APPENDED 1.6.0).
	 * Operator ruling 2026-09-25, HOLD WITH A DECLARED LIMIT: it becomes MASTER on the
	 * source's first master-only frame, BOX on a box-only frame or a declared model, and
	 * BOX once REAC_DESK_PROOF_WINDOW_NS passes with neither (reac_arbitration.h). Width
	 * never decides it: a box may be 40 wide. A tap does not serve a stream in this
	 * state — it has not been told whose audio it is. */
	REAC_TAP_STREAM_UNRESOLVED,
};

/* One heard stream. `kind` and `channels` are read off the GEOMETRY — reac.h's
 * "THE GEOMETRY IS THE ROLE": the 40-ch solution (1492 B clean) is the master's
 * downstream and nothing else, every legal `52 + n*36` is a box return of width n, and
 * the wire declares the width so nothing configures or remembers it.
 *
 * `model_index` is the separate fact the box's own CONFIG-ANNOUNCE carries (a byte-exact
 * match through reac_ctrl_identify_box; -1 until one arrives). It names the MODEL, and
 * the model's nominal input width lands in `announced_channels`. When the two widths
 * disagree, `width_disagrees` says so and `channels` still wins: a tap serves what is on
 * the wire, and a declaration that does not match the frames is a fact to report, not a
 * width to adopt. */
struct reac_tap_stream {
	enum reac_tap_stream_kind kind;
	uint8_t  src[6];
	unsigned channels;            /* from the frame geometry; 40 for the master */
	int      model_index;         /* reac_disco_model_index of the announce, or -1 */
	unsigned announced_channels;  /* the announced model's in_ch, or 0 */
	int      width_disagrees;     /* announce width != frame width, both known */

	/* Frame accounting, per stream. A mirror twin lands in `dups`, NEVER in `gaps`:
	 * the guard runs before the counter is compared, so the repeated counter is never
	 * seen as a jump. `gaps` therefore means frames that genuinely did not arrive. */
	uint64_t frames;              /* accepted after the duplicate guard */
	uint64_t dups;                /* byte-identical repeats dropped (mirror twin / over-clock) */
	uint64_t gaps;                /* frames inferred lost from counter jumps */

	uint64_t first_ts_usec;       /* capture time of this stream's first accepted frame */
	uint64_t last_ts_usec;        /* ... and of its most recent one */

	/* private survey state — never read by a caller */
	uint16_t last_counter;
	int      have_counter;
	size_t   prev_clean_len;
	int      have_prev;
	uint8_t  prev[REAC_FRAME_BYTES + 64];
};

/* The LISTEN half: a pure frame sink. No socket, no clock, no allocation — feed it
 * frames from anywhere (a live capture, a pcap replay, a test) and it builds the roster.
 * This is what makes the tap's classification testable against a recorded capture
 * without a wire. */
struct reac_tap_survey {
	struct reac_tap_stream stream[REAC_TAP_MAX_STREAMS];
	unsigned n;
	uint64_t frames_seen;      /* 0x8819 frames offered */
	uint64_t frames_ungeometric; /* 0x8819 but no legal geometry (control-only shapes) */
	uint64_t frames_overflow;  /* refused because the roster is full — reported, never silent */
	uint64_t frames_self;      /* refused as our own host's echo — see reac_tap_survey_set_self */

	uint8_t  self_mac[6];
	int      have_self;
};

void reac_tap_survey_init(struct reac_tap_survey *s);

/* OUR OWN MAC, so a frame this host put on the wire is never taken for a peer.
 *
 * A tap transmits nothing, so in the tap role this filter should have nothing to catch —
 * and it catches something the moment the console was ever anything else. The recorded
 * M-200 + S-1608 mirror of 2026-09-11 carries THREE upstream talkers: the desk, the
 * S-1608, and 34:5a:60:9f:9e:be, which is this host's own NIC, because reac-pw was
 * running as a SLAVE while the capture was taken. Unfiltered, a tap over that segment
 * would mint a third box stream and serve the console its own microphones back. Same law
 * as reac_disco's self-filter, for the same reason: PACKET_IGNORE_OUTGOING is
 * best-effort, and a mirror re-delivers our transmit side by construction.
 *
 * Optional: with no self MAC set, nothing is filtered (and the roster says what it heard). */
void reac_tap_survey_set_self(struct reac_tap_survey *s, const uint8_t mac[6]);

/* Offer one full ethernet frame. `ts_usec` is its capture time (0 if unknown — the rate
 * estimate then stays unavailable rather than being guessed).
 *
 * Returns the index of the stream it was accepted into, or:
 *   REAC_TAP_NOT_REAC   not a 0x8819 frame, or no legal REAC geometry
 *   REAC_TAP_DUPLICATE  a byte-identical repeat of this stream's previous frame
 *   REAC_TAP_FULL       the roster is full and this is a new source
 *   REAC_TAP_SELF       this host's own echo (reac_tap_survey_set_self)
 * A BROADCAST audio frame from a new source mints an UNRESOLVED stream (the hold); a
 * UNICAST one mints a BOX stream, at any box width.
 * A negative return is a REFUSAL with a reason, never a silent drop. */
#define REAC_TAP_NOT_REAC   (-1)
#define REAC_TAP_DUPLICATE  (-2)
#define REAC_TAP_FULL       (-3)
#define REAC_TAP_SELF       (-4)
int reac_tap_survey_frame(struct reac_tap_survey *s, const uint8_t *frame, size_t len,
                          uint64_t ts_usec);

/* The sample rate the MASTER's cadence means, snapped by reac_rate_snap; 0 when no
 * master was heard, when fewer than two of its frames were accepted, or when the frames
 * carried no timestamps. Absence is 0 and 0 is never a rate. */
int reac_tap_survey_rate(const struct reac_tap_survey *s);

/* Apply the HOLD's limit at `now_usec` (the survey's timestamp clock): every UNRESOLVED
 * stream first heard REAC_DESK_PROOF_WINDOW_NS or more before `now_usec` without a
 * master-only frame becomes a BOX. Returns how many are still UNRESOLVED. A frame's own
 * timestamp applies the same limit as it arrives; this is for the end of a survey. */
unsigned reac_tap_survey_resolve(struct reac_tap_survey *s, uint64_t now_usec);

/* The master stream, or NULL. At most one exists: two masters on one segment is the
 * condition arbitration §2 forbids, and the survey reports the second as its own BOX-less
 * MASTER entry rather than merging it (the roster is the evidence; the judgement is the
 * caller's). */
const struct reac_tap_stream *reac_tap_survey_master(const struct reac_tap_survey *s);
/* The box stream for this source MAC, or NULL. */
const struct reac_tap_stream *reac_tap_survey_box(const struct reac_tap_survey *s,
                                                  const uint8_t mac[6]);

/* --- the SERVE half ------------------------------------------------------------- */

struct reac_tap_cfg {
	enum reac_rx_kind kind;   /* REAC_RX_LIVE (ifname) or REAC_RX_PCAP (file path) */
	const char *source;
	int forced_rate;          /* 0 = take the master's measured cadence */
	int survey_ms;            /* LIVE: how long to listen before serving. 0 -> 1000 */
	uint32_t survey_frames;   /* PCAP: frames to read before serving. 0 -> 20000 */
	const uint8_t *self_mac;  /* our own NIC's MAC, or NULL — see reac_tap_survey_set_self */
};

struct reac_tap {
	struct reac_tap_cfg cfg;
	struct reac_tap_survey survey;
	int sample_rate;          /* measured from the master, or cfg.forced_rate */
	unsigned n;               /* streams being served == survey.n */
	struct reac_ring ring[REAC_TAP_MAX_STREAMS];
	struct reac_rx   rx[REAC_TAP_MAX_STREAMS];
	int started;
};

/* Listen on the source, classify what is heard, and size one ring per stream. Opens NO
 * transmit path of any kind. Returns 0, or -1 when the source cannot be read. A source
 * that yields no REAC frame at all opens with n == 0 — an empty roster is a fact, and
 * the caller reports "heard nothing" rather than serving silence under a made-up name. */
int reac_tap_open(struct reac_tap *t, const struct reac_tap_cfg *cfg);

unsigned reac_tap_stream_count(const struct reac_tap *t);
const struct reac_tap_stream *reac_tap_stream_at(const struct reac_tap *t, unsigned i);
struct reac_ring *reac_tap_ring_at(struct reac_tap *t, unsigned i);

/* Spawn one reac_rx feeder per stream, each gated to that stream alone (the master's by
 * geometry, a box's by its source MAC through reac_rx_peer_reset). Returns 0 / -1. */
int reac_tap_start(struct reac_tap *t);
void reac_tap_stop(struct reac_tap *t);
void reac_tap_close(struct reac_tap *t);

#endif /* REAC_TAP_H */
