// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_tap — the passive role. See <reac/transport/reac_tap.h> for what it is and why
 * it is not reac_slave with transmit disabled.
 *
 * NOTHING IN THIS FILE REACHES THE WIRE OUTBOUND. It opens a capture (or a pcap file),
 * classifies, and hands each stream to a reac_rx feeder. There is no reac_tx, no
 * reac_pacer, no reac_slave, no reac_seglock and no send/sendto here, and
 * tools/conformance-tap-silent.sh fails the build if one appears — with reac_slave.c as
 * its positive control, so a scan that stopped matching cannot report silence.
 */
#define _DEFAULT_SOURCE
#include <reac/transport/reac_tap.h>

#include <reac/reac_capture.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_disco.h>
#include <reac/reac_arbitration.h>   /* the hold's limit: one master-only cadence */
#include <reac/reac_upstream.h>
#include <reac/pcap_source.h>

#include <string.h>
#include <stdio.h>
#include <time.h>

#define REAC_TAP_DEFAULT_SURVEY_MS      1000u
#define REAC_TAP_DEFAULT_SURVEY_FRAMES 20000u

static uint64_t mono_usec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

void reac_tap_survey_init(struct reac_tap_survey *s)
{
	if (s)
		memset(s, 0, sizeof *s);
}

void reac_tap_survey_set_self(struct reac_tap_survey *s, const uint8_t mac[6])
{
	if (!s)
		return;
	if (!mac) {
		s->have_self = 0;
		return;
	}
	memcpy(s->self_mac, mac, 6);
	s->have_self = 1;
}

/* Find the stream this source+geometry belongs to, minting one if there is room.
 * Returns an index, or REAC_TAP_FULL. Keyed on (src MAC, kind) rather than the MAC
 * alone: a box strapped to master mode broadcasts a BOX geometry while its control
 * frames claim master (reac.h, "THE GEOMETRY IS THE ROLE"), and if such a peer ever
 * emitted both shapes they are two streams of different widths, never one. */
static int stream_for(struct reac_tap_survey *s, const uint8_t src[6],
                      enum reac_tap_stream_kind kind, unsigned channels)
{
	/* ONE STREAM PER SOURCE. A broadcast stream's kind is resolved after it is minted
	 * (UNRESOLVED -> MASTER/BOX), so the lookup is by source alone. */
	for (unsigned i = 0; i < s->n; i++)
		if (memcmp(s->stream[i].src, src, 6) == 0)
			return (int)i;
	if (s->n >= REAC_TAP_MAX_STREAMS) {
		s->frames_overflow++;
		return REAC_TAP_FULL;
	}
	struct reac_tap_stream *st = &s->stream[s->n];
	memset(st, 0, sizeof *st);
	st->kind = kind;
	memcpy(st->src, src, 6);
	st->channels = channels;
	st->model_index = -1;
	return (int)s->n++;
}

/* THE HOLD, RESOLVED BY THE SOURCE'S OWN FRAMES: a master-only frame (cfea announce,
 * slot map, head-amp record, scene push) makes it the desk; a box-only frame or a
 * declared model makes it a box. The same verdicts reac_disco draws, from the same
 * classifier. */
static void resolve_by_role(const struct reac_tap_survey *s, struct reac_tap_stream *st,
                            const uint8_t *frame, size_t clean)
{
	if (st->kind != REAC_TAP_STREAM_UNRESOLVED)
		return;
	static const uint8_t NONE[6] = { 0 };
	struct reac_disco_sighting sg;
	if (reac_disco_classify(frame, clean, s->have_self ? s->self_mac : NONE, &sg) != 0)
		return;
	if (sg.model != NULL || sg.role == REAC_DISCO_ROLE_BOX || st->model_index >= 0)
		st->kind = REAC_TAP_STREAM_BOX;
	else if (sg.role == REAC_DISCO_ROLE_MASTER)
		st->kind = REAC_TAP_STREAM_MASTER;
}

/* The stream's own pace, from its counter advance over its timestamps; 0 when there is
 * no span to measure it over. */
static int stream_fps(const struct reac_tap_stream *st)
{
	const uint64_t advance = st->frames + st->gaps;
	if (advance < 2 || !st->first_ts_usec || st->last_ts_usec <= st->first_ts_usec)
		return 0;
	return (int)((double)(advance - 1) * 1e6 / (double)(st->last_ts_usec - st->first_ts_usec) + 0.5);
}

/* THE HOLD'S LIMIT (reac_arbitration.h): ONE MASTER-ONLY CADENCE, IN FRAMES AT THE
 * CURRENT RATE. The sender's own counter says how many frames it has put on the wire
 * since it was first heard — lost ones included, a mirror twin never (the duplicate guard
 * runs first) — and a broadcast stream that has advanced a whole cadence at its own pace
 * with no master-only op is a box. With no pace measurable yet the widest frame count of
 * the three paces is the bar: a hold that is too short at some pace is not a hold. */
static void resolve_by_frames(struct reac_tap_stream *st)
{
	if (st->kind != REAC_TAP_STREAM_UNRESOLVED)
		return;
	const int fps = stream_fps(st);
	const uint32_t window = fps > 0 ? reac_master_only_cadence_frames(fps)
	                                : REAC_MASTER_ONLY_CADENCE_FRAMES_96K;
	if (st->frames + st->gaps > window)
		st->kind = REAC_TAP_STREAM_BOX;
}

/* The same limit at the END of a survey, with no frame to count: the time since the
 * stream was first heard, as frames at its own pace (or the longest window, unmeasured). */
static void resolve_by_time(struct reac_tap_stream *st, uint64_t now_usec)
{
	if (st->kind != REAC_TAP_STREAM_UNRESOLVED || !st->first_ts_usec)
		return;
	if (now_usec > st->first_ts_usec &&
	    (now_usec - st->first_ts_usec) * 1000ull >= reac_master_only_cadence_ns(stream_fps(st)))
		st->kind = REAC_TAP_STREAM_BOX;
}

unsigned reac_tap_survey_resolve(struct reac_tap_survey *s, uint64_t now_usec)
{
	unsigned left = 0;
	if (!s)
		return 0;
	for (unsigned i = 0; i < s->n; i++) {
		resolve_by_time(&s->stream[i], now_usec);
		left += s->stream[i].kind == REAC_TAP_STREAM_UNRESOLVED;
	}
	return left;
}

/* The box's own CONFIG-ANNOUNCE, if this frame carries one: a byte-exact match against
 * the fixed model matrix, never reac_box_model_by_channels (whose S-1608 default would
 * name a box that was never identified — reac_disco.h says why). Recorded once; the
 * announced width is compared with the geometry rather than replacing it. */
static void note_model(struct reac_tap_stream *st, const uint8_t *frame, size_t len)
{
	if (st->model_index >= 0)
		return;
	const struct reac_box_model *m = reac_ctrl_identify_box(frame, len);
	if (!m)
		return;
	st->model_index = reac_disco_model_index(m);
	st->announced_channels = (unsigned)(m->in_ch > 0 ? m->in_ch : 0);
	if (st->announced_channels && st->channels)
		st->width_disagrees = st->announced_channels != st->channels;
}

int reac_tap_survey_frame(struct reac_tap_survey *s, const uint8_t *frame, size_t len,
                          uint64_t ts_usec)
{
	if (!s || !frame || !reac_frame_is_reac(frame, len))
		return REAC_TAP_NOT_REAC;
	s->frames_seen++;

	/* THE DOOR STRIPS THE CAPTURE PATH'S +2, once, for everything below — this
	 * survey exists to read MIRRORED captures, which is exactly where the residue
	 * lives (<reac/reac.h>, census 2026-09-21). No parser behind this line
	 * tolerates a residue length, so `clean` is what they are all given. */
	const size_t clean = reac_frame_clean_len(len);
	const uint8_t *src = frame + 6;

	if (s->have_self && memcmp(src, s->self_mac, 6) == 0) {
		/* Our own echo. Never a peer, never a stream, never audio we serve back to
		 * ourselves — see reac_tap_survey_set_self for the capture that proves this
		 * filter has something to catch. */
		s->frames_self++;
		return REAC_TAP_SELF;
	}

	enum reac_tap_stream_kind kind;
	unsigned channels;
	/* DIRECTION BEFORE WIDTH, AND ROLE BEFORE BOTH. A master BROADCASTS its downstream;
	 * a box UNICASTS its return to its master, at any box width up to the whole fabric
	 * (operator ruling 2026-09-25) — so a unicast stream is a box's. A BROADCAST stream
	 * is the desk's downstream OR a box's presence-flood, and nothing in its length says
	 * which: it is minted UNRESOLVED and the source's own frames resolve it (below),
	 * or the declared window does (resolve_by_time). */
	static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	const int unicast = memcmp(frame, BCAST, 6) != 0;
	const int width = reac_upstream_channels(clean);
	if (width > 0) {
		kind = unicast ? REAC_TAP_STREAM_BOX : REAC_TAP_STREAM_UNRESOLVED;
		channels = (unsigned)width;
	} else {
		/* A control-only shape: no audio geometry, so it cannot MINT a stream — a
		 * stream whose width nothing declared would be a node of invented size. It
		 * can still IDENTIFY a peer already heard: the config-announce names the
		 * model of a box the data frames already sized, and a master-only frame
		 * resolves a broadcast stream to the desk. */
		s->frames_ungeometric++;
		for (unsigned i = 0; i < s->n; i++)
			if (memcmp(s->stream[i].src, src, 6) == 0) {
				note_model(&s->stream[i], frame, clean);
				resolve_by_role(s, &s->stream[i], frame, clean);
			}
		return REAC_TAP_NOT_REAC;
	}

	const int idx = stream_for(s, src, kind, channels);
	if (idx < 0)
		return idx;
	struct reac_tap_stream *st = &s->stream[idx];

	/* THE DUPLICATE GUARD, PER STREAM, AND BEFORE THE COUNTER IS READ. A mirrored port
	 * delivers each transiting frame twice; the two copies differ only in the 2 bytes
	 * of Ethernet FCS residue one of them kept, so the compare runs on the CLEAN length
	 * (reac_rx.h carries the corpus evidence: 61 of 83 captures show the twin). Running
	 * it here rather than after the counter is what keeps a mirror copy out of `gaps`:
	 * a repeated counter never reaches reac_counter_gap at all.
	 *
	 * PER STREAM is the other half. The master's and the boxes' frames interleave on a
	 * mirror, so a single previous-frame slot would be overwritten between a twin pair
	 * and match nothing. reac_rx gets this for free by running one instance per stream;
	 * the survey has to hold one slot per stream to match it. */
	if (st->have_prev && clean == st->prev_clean_len && clean <= sizeof st->prev &&
	    memcmp(frame, st->prev, clean) == 0) {
		st->dups++;
		return REAC_TAP_DUPLICATE;
	}
	if (clean <= sizeof st->prev) {
		memcpy(st->prev, frame, clean);
		st->prev_clean_len = clean;
		st->have_prev = 1;
	}

	const uint16_t counter = reac_frame_counter(frame);
	if (st->have_counter)
		st->gaps += reac_counter_gap(st->last_counter, counter);
	st->last_counter = counter;
	st->have_counter = 1;

	if (ts_usec) {
		if (!st->first_ts_usec)
			st->first_ts_usec = ts_usec;
		st->last_ts_usec = ts_usec;
	}
	st->frames++;
	note_model(st, frame, clean);
	resolve_by_role(s, st, frame, clean);
	resolve_by_frames(st);
	return idx;
}

const struct reac_tap_stream *reac_tap_survey_master(const struct reac_tap_survey *s)
{
	if (!s)
		return NULL;
	for (unsigned i = 0; i < s->n; i++)
		if (s->stream[i].kind == REAC_TAP_STREAM_MASTER)
			return &s->stream[i];
	return NULL;
}

const struct reac_tap_stream *reac_tap_survey_box(const struct reac_tap_survey *s,
                                                  const uint8_t mac[6])
{
	if (!s || !mac)
		return NULL;
	for (unsigned i = 0; i < s->n; i++)
		if (s->stream[i].kind == REAC_TAP_STREAM_BOX &&
		    memcmp(s->stream[i].src, mac, 6) == 0)
			return &s->stream[i];
	return NULL;
}

int reac_tap_survey_rate(const struct reac_tap_survey *s)
{
	const struct reac_tap_stream *m = reac_tap_survey_master(s);
	if (!m)
		return 0;
	/* The counter ADVANCE, not the frame count: a frame that did not arrive still
	 * happened on the wire and the cadence is about the wire. Undercounting it would
	 * read a lossy 48 kHz segment as 44.1. */
	const uint64_t advance = m->frames - 1 + m->gaps;
	if (m->frames < 2 || !m->first_ts_usec || m->last_ts_usec <= m->first_ts_usec)
		return 0;
	const double span_us = (double)(m->last_ts_usec - m->first_ts_usec);
	return reac_rate_snap((double)advance * 1e6 / span_us);
}

/* --- the SERVE half -------------------------------------------------------------- */

/* Listen once, off the same two sources reac_rx serves from. NO TX socket is opened in
 * either arm: reac_capture_open is a receive path (its own header says so) and
 * pcap_source is a file. */
static int survey_source(struct reac_tap *t)
{
	uint8_t frame[REAC_FRAME_BYTES + 64];

	if (t->cfg.kind == REAC_RX_PCAP) {
		struct pcap_source ps = { 0 };
		if (pcap_source_open(&ps, t->cfg.source) != 0) {
			fprintf(stderr, "reac_tap: cannot read '%s'\n", t->cfg.source);
			return -1;
		}
		uint32_t budget = t->cfg.survey_frames ? t->cfg.survey_frames
		                                       : REAC_TAP_DEFAULT_SURVEY_FRAMES;
		uint64_t last_ts = 0;
		for (uint32_t i = 0; i < budget; i++) {
			uint64_t ts = 0;
			long n = pcap_source_next(&ps, frame, sizeof frame, &ts);
			if (n == 0)
				break;          /* EOF: the survey is the whole file */
			if (n < 0)
				continue;
			reac_tap_survey_frame(&t->survey, frame, (size_t)n, ts);
			if (ts > last_ts)
				last_ts = ts;
		}
		pcap_source_close(&ps);
		/* The recording is all the evidence there is: apply the hold's limit at its
		 * last timestamp. A broadcast stream shorter than the window stays unresolved. */
		reac_tap_survey_resolve(&t->survey, last_ts);
		return 0;
	}

	struct reac_capture cap = { .fd = -1 };
	if (reac_capture_open(&cap, t->cfg.source) != 0) {
		fprintf(stderr, "reac_tap: --live '%s': no such interface, or insufficient "
		        "capability (needs CAP_NET_RAW) — a tap that cannot receive has no "
		        "job at all\n", t->cfg.source);
		return -1;
	}
	reac_capture_set_nonblock(&cap, 1);
	const unsigned ms = t->cfg.survey_ms ? (unsigned)t->cfg.survey_ms
	                                     : REAC_TAP_DEFAULT_SURVEY_MS;
	const uint64_t deadline = mono_usec() + (uint64_t)ms * 1000ull;
	/* THE HOLD HAS A DECLARED LIMIT, AND THE SURVEY WAITS FOR IT: past the ordinary
	 * survey, keep listening while any broadcast stream is still unresolved, for at most
	 * one master-only cadence more — a desk proves itself on its next master-only op, a
	 * flood-only box by the cadence running out. */
	const uint64_t hold_end = deadline + reac_master_only_cadence_ns(0) / 1000ull;
	while (mono_usec() < deadline ||
	       (mono_usec() < hold_end && reac_tap_survey_resolve(&t->survey, mono_usec()) > 0)) {
		long n = reac_capture_next(&cap, frame, sizeof frame);
		if (n <= 0) {
			struct timespec idle = { 0, 1000000 };  /* 1 ms */
			nanosleep(&idle, NULL);
			continue;
		}
		reac_tap_survey_frame(&t->survey, frame, (size_t)n, mono_usec());
	}
	reac_capture_close(&cap);
	return 0;
}

int reac_tap_open(struct reac_tap *t, const struct reac_tap_cfg *cfg)
{
	if (!t || !cfg || !cfg->source)
		return -1;
	memset(t, 0, sizeof *t);
	t->cfg = *cfg;
	reac_tap_survey_init(&t->survey);
	reac_tap_survey_set_self(&t->survey, cfg->self_mac);

	if (survey_source(t) != 0)
		return -1;

	/* A stream still UNRESOLVED has not proven whose audio it is: it is not served
	 * (the hold), and saying so is the whole report. */
	unsigned kept = 0, held = 0;
	for (unsigned i = 0; i < t->survey.n; i++) {
		if (t->survey.stream[i].kind == REAC_TAP_STREAM_UNRESOLVED) {
			held++;
			continue;
		}
		if (kept != i)
			t->survey.stream[kept] = t->survey.stream[i];
		kept++;
	}
	t->survey.n = kept;
	if (held)
		fprintf(stderr, "reac_tap: '%s': %u broadcast stream(s) never proved desk or box "
		        "inside the declared window — not served\n", t->cfg.source, held);

	t->sample_rate = cfg->forced_rate ? cfg->forced_rate : reac_tap_survey_rate(&t->survey);
	t->n = t->survey.n;

	if (t->n == 0)
		return 0;   /* heard nothing. An empty roster is a fact, not a failure. */

	if (t->sample_rate <= 0) {
		/* No master heard and no rate forced. A tap locks to the master's cadence the
		 * way a slave does, and there is no honest second source for it — a box's own
		 * pace is the master's, so with no master there is nothing to read it from.
		 * Refuse rather than publish a default nobody measured. */
		fprintf(stderr, "reac_tap: '%s': %u stream(s) heard but NO MASTER cadence to "
		        "take the rate from — refusing to serve at a rate nothing measured\n",
		        t->cfg.source, t->n);
		return -1;
	}

	for (unsigned i = 0; i < t->n; i++) {
		struct reac_rx_cfg rc = {
			.kind = t->cfg.kind,
			.source = t->cfg.source,
			.forced_rate = t->sample_rate,
			.pcap_realtime = t->cfg.kind == REAC_RX_PCAP ? 1 : 0,
			.accept = t->survey.stream[i].kind == REAC_TAP_STREAM_MASTER
			                ? REAC_RX_ACCEPT_DOWNSTREAM : REAC_RX_ACCEPT_UPSTREAM,
		};
		if (reac_rx_open(&t->rx[i], &rc, &t->ring[i]) != 0) {
			for (unsigned j = 0; j < i; j++)
				reac_ring_free(&t->ring[j]);
			t->n = 0;
			return -1;
		}
		/* PIN the gate to THIS source. Left to latch on its own, an upstream feeder
		 * takes whichever box it hears first, so two boxes on a mirrored segment would
		 * race for both rings and one box would be served twice; and a downstream feeder
		 * would take a 40-wide box's broadcast flood beside the desk's frames. */
		reac_rx_peer_reset(&t->rx[i], t->survey.stream[i].src, 1);
	}
	return 0;
}

unsigned reac_tap_stream_count(const struct reac_tap *t)
{
	return t ? t->n : 0;
}

const struct reac_tap_stream *reac_tap_stream_at(const struct reac_tap *t, unsigned i)
{
	return (t && i < t->n) ? &t->survey.stream[i] : NULL;
}

struct reac_ring *reac_tap_ring_at(struct reac_tap *t, unsigned i)
{
	return (t && i < t->n) ? &t->ring[i] : NULL;
}

int reac_tap_start(struct reac_tap *t)
{
	if (!t || t->n == 0 || t->started)
		return -1;
	for (unsigned i = 0; i < t->n; i++) {
		if (reac_rx_start(&t->rx[i]) != 0) {
			for (unsigned j = 0; j < i; j++)
				reac_rx_stop(&t->rx[j]);
			return -1;
		}
	}
	t->started = 1;
	return 0;
}

void reac_tap_stop(struct reac_tap *t)
{
	if (!t || !t->started)
		return;
	for (unsigned i = 0; i < t->n; i++)
		reac_rx_stop(&t->rx[i]);
	t->started = 0;
}

void reac_tap_close(struct reac_tap *t)
{
	if (!t)
		return;
	reac_tap_stop(t);
	for (unsigned i = 0; i < t->n; i++) {
		reac_rx_close(&t->rx[i]);
		reac_ring_free(&t->ring[i]);
	}
	t->n = 0;
}
