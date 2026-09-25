// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test: reac_tap — the PASSIVE role's classification, its rate detection and its
 * mirror-duplicate accounting.
 *
 * A tap only exists on a SWITCH MIRROR (plug-and-play §1, scenario B), and a mirrored
 * port delivers every transiting frame TWICE — one copy keeping 2 bytes of the frame's
 * own Ethernet FCS after the C2 EA marker, the other not. So the fixture here is a
 * mirrored segment, not a clean one: master 1492 + its 1494 twin and box 628 + its 630
 * twin, interleaved the way a real mirror interleaves them.
 *
 * THE ASSERTION THAT COSTS: a twin must land in `dups` and NEVER in `gaps`. A survey
 * that compared counters before collapsing the copy would read every repeated counter
 * as a jump, and a survey keeping ONE previous-frame slot across all streams would miss
 * every twin the other stream's frame separated. Both are checked, and both have a
 * POSITIVE CONTROL beside them so a counter that has stopped counting cannot pass as a
 * clean segment: three counters are deliberately skipped and `gaps` must read exactly 3.
 *
 * The rate arm has the same shape. The fixture is stamped at 4000 pps and must read
 * 48000 — which on its own would also be true of a hardcoded default, so the same
 * fixture is re-stamped at 8000 and 3675 pps and must read 96000 and 44100.
 *
 * THE CORPUS ARM (argv[1]) is the real thing: the recorded M-200 + S-1608 segment of
 * 2026-09-11 (switch port 7 mirroring port 4, VLAN 12), tags stripped with
 * `tcprewrite --enet-vlan=del`. MEASURED 2026-09-13 over its first 60 000 records:
 *
 *   00:40:ab:c9:cc:03  master  40 ch  19933 frames  0 dups  1 gap   <- the M-200
 *   00:40:ab:c4:80:3b  box     16 ch  19935 frames  0 dups  0 gaps  <- the S-1608
 *   34:5a:60:9f:9e:be  box     16 ch   3830 frames                  <- THIS HOST
 *   rate: 48000 Hz, from the master's cadence
 *
 * Two things that capture taught, both now assertions here:
 *
 *   1. IT CARRIES NO TWIN. Both wire lengths are present (1492 and 1494; 628 and 630)
 *      but never as adjacent same-counter pairs — 0 byte-identical repeats in 60 000
 *      records. So the corpus cannot prove the duplicate guard on its own, and this arm
 *      does not pretend it does: it applies the MIRROR TRANSFORM to those real frames
 *      (re-emit each one with 2 bytes of FCS residue, which is exactly what a
 *      both-directions mirror delivers) and requires every copy to land in `dups` while
 *      the one real gap survives untouched.
 *   2. THE THIRD TALKER IS US. 34:5a:60:9f:9e:be is this host's own NIC: reac-pw was
 *      running as a SLAVE when the capture was taken. Unfiltered it mints a third box
 *      stream and a tap would serve the console its own inputs back. The self-filter is
 *      asserted BOTH ways here — three streams without it, two with it — so the filter
 *      is proven to have something to catch rather than proven to be harmless.
 *
 * Absent a readable capture, that arm is reported as NOT RUN, loudly: a skipped arm is
 * not a pass.
 */
#define _DEFAULT_SOURCE
#include "reac/transport/reac_tap.h"
#include "reac/pcap_source.h"
#include "reac/reac.h"
#include "reac/reac_arbitration.h"   /* the hold's limit: one master-only cadence */
#include "reac/transport/reac_segment_ident.h"

#include "ctrl_fixtures.inc"          /* FX_ANNOUNCE — a captured cfea master announce */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static const uint8_t MASTER_MAC[6] = { 0x00, 0x40, 0xab, 0xc9, 0xcc, 0x03 };
static const uint8_t BOX_MAC[6]    = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x3b };
static const uint8_t BCAST[6]      = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

#define BOX_CH     16
#define BOX_LEN    (REAC_UPSTREAM_OVERHEAD + BOX_CH * REAC_UPSTREAM_BYTES_PER_CH) /* 628 */
#define TICKS      200
#define GAP_AT     120   /* counters skipped here, so `gaps` has something real to find */
#define GAP_SIZE   3

/* A REAC-shaped frame: dst, src, 0x8819, LE counter at 14, body keyed by the counter so
 * two different counters are never byte-identical, C2 EA at the clean end. */
static void mk_frame(uint8_t *f, size_t clean, const uint8_t *dst, const uint8_t *src,
                     uint16_t counter)
{
	memset(f, (uint8_t)(counter & 0xff), clean);
	memcpy(f, dst, 6);
	memcpy(f + 6, src, 6);
	f[12] = 0x88; f[13] = 0x19;
	f[14] = (uint8_t)(counter & 0xff);
	f[15] = (uint8_t)(counter >> 8);
	f[clean - 2] = 0xC2; f[clean - 1] = 0xEA;
}

static void wr_u32(FILE *f, uint32_t v)
{
	uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
	fwrite(b, 1, 4, f);
}

static void wr_rec(FILE *f, const uint8_t *buf, size_t len, uint64_t ts_usec)
{
	wr_u32(f, (uint32_t)(ts_usec / 1000000ull));
	wr_u32(f, (uint32_t)(ts_usec % 1000000ull));
	wr_u32(f, (uint32_t)len);
	wr_u32(f, (uint32_t)len);
	fwrite(buf, 1, len, f);
}

/* A MIRRORED segment at `pps`: per tick, the master's downstream twinned (1492 + 1494)
 * and the box's return NOT twinned (628 only) — the real split a both-directions mirror
 * produces, and the one that makes the per-stream previous-frame slot load-bearing: the
 * two master copies are never adjacent in the file. */
static void write_mirror_pcap(const char *path, double pps)
{
	FILE *f = fopen(path, "wb");
	wr_u32(f, 0xA1B2C3D4u); fwrite("\x02\x00\x04\x00", 1, 4, f);
	wr_u32(f, 0); wr_u32(f, 0); wr_u32(f, 65535); wr_u32(f, 1);

	uint8_t dn[REAC_FRAME_BYTES + 2], up[BOX_LEN];
	uint16_t counter = 7;
	const double step_us = 1e6 / pps;
	for (int i = 0; i < TICKS; i++) {
		uint64_t ts = 1000000ull + (uint64_t)(i * step_us);
		mk_frame(dn, REAC_FRAME_BYTES, BCAST, MASTER_MAC, counter);
		/* THE DESK ANNOUNCES ITSELF (a captured cfea over its first frame's control
		 * block): a broadcast stream is the desk's because it says so, never because
		 * it is 40 wide (operator ruling 2026-09-25) — a desk that never announced
		 * would be held, and then served as a box. */
		if (i == 0)
			memcpy(dn + 16, FX_ANNOUNCE, sizeof FX_ANNOUNCE);
		mk_frame(up, BOX_LEN, MASTER_MAC, BOX_MAC, counter);
		wr_rec(f, dn, REAC_FRAME_BYTES, ts);        /* master, first copy   */
		wr_rec(f, up, BOX_LEN, ts + 20);            /* box return in between */
		dn[REAC_FRAME_BYTES] = 0x5a; dn[REAC_FRAME_BYTES + 1] = 0xc3;  /* FCS residue */
		wr_rec(f, dn, REAC_FRAME_BYTES + 2, ts + 40); /* master, MIRROR TWIN */
		counter = (uint16_t)(counter + (i == GAP_AT ? 1 + GAP_SIZE : 1));
	}
	fclose(f);
}

/* Feed a file through the survey. `mirror` offers every frame TWICE — once at its clean
 * length and once with 2 bytes of FCS residue after the end marker — which is exactly the
 * pair a both-directions port mirror delivers.
 *
 * It normalizes to the CLEAN length first, and that detail is the whole trick: the corpus
 * already holds frames of both wire lengths (1492 and 1494), so appending 2 bytes blindly
 * turns a recorded 1494 into a 1496 that matches no geometry at all and is refused as
 * ungeometric rather than collapsed as a twin. A transform that manufactures a malformed
 * frame and then reports the guard "missed" it is measuring itself. */
static void survey_file_mirrored(const char *path, struct reac_tap_survey *s,
                                 uint32_t budget, int mirror)
{
	struct pcap_source ps = { 0 };
	if (pcap_source_open(&ps, path) != 0) {
		printf("FAIL: cannot open %s\n", path);
		fails++;
		return;
	}
	uint8_t buf[REAC_FRAME_BYTES + 64];
	for (uint32_t i = 0; i < budget; i++) {
		uint64_t ts = 0;
		long n = pcap_source_next(&ps, buf, sizeof buf, &ts);
		if (n == 0)
			break;
		if (n < 0)
			continue;
		if (!mirror) {
			reac_tap_survey_frame(s, buf, (size_t)n, ts);
			continue;
		}
		size_t clean = reac_frame_clean_len((size_t)n);
		if (clean + 2 > sizeof buf)
			continue;
		reac_tap_survey_frame(s, buf, clean, ts);
		buf[clean] = 0x5a; buf[clean + 1] = 0xc3;
		reac_tap_survey_frame(s, buf, clean + 2, ts);
	}
	pcap_source_close(&ps);
}

static void survey_file(const char *path, struct reac_tap_survey *s, uint32_t budget)
{
	survey_file_mirrored(path, s, budget, 0);
}

static void arm_synthetic(const char *dir)
{
	char path[512];
	snprintf(path, sizeof path, "%s/mirror48.pcap", dir);
	write_mirror_pcap(path, 4000.0);

	struct reac_tap_survey s;
	reac_tap_survey_init(&s);
	survey_file(path, &s, 100000);

	CHECK(s.n == 2, "a mirrored segment with one master and one box yields two streams");

	const struct reac_tap_stream *m = reac_tap_survey_master(&s);
	CHECK(m != NULL, "the 1492 B broadcast is classified as the master's downstream");
	if (m) {
		CHECK(m->channels == 40, "the master's downstream is 40 channels");
		CHECK(memcmp(m->src, MASTER_MAC, 6) == 0, "the master stream carries the master's MAC");
		CHECK(m->frames == TICKS, "every master tick is accepted exactly once");
		/* THE DEFECT THIS TEST EXISTS FOR. */
		CHECK(m->dups == TICKS, "every mirror twin is counted as a duplicate");
		CHECK(m->gaps == GAP_SIZE, "a mirror twin is NEVER a gap; only the real hole counts");
	}

	const struct reac_tap_stream *b = reac_tap_survey_box(&s, BOX_MAC);
	CHECK(b != NULL, "the box's 628 B return is classified as a box stream by its MAC");
	if (b) {
		CHECK(b->channels == BOX_CH, "the box stream is 16 channels, from the frame geometry");
		CHECK(b->kind == REAC_TAP_STREAM_BOX, "the box stream is a box stream");
		CHECK(b->frames == TICKS, "every box tick is accepted");
		CHECK(b->dups == 0, "the unicast return crosses the mirror once: no twin");
		CHECK(b->gaps == GAP_SIZE, "the box's own hole is counted, and only that");
	}
	CHECK(reac_tap_survey_box(&s, MASTER_MAC) == NULL,
	      "the master's MAC names no box stream");

	/* THE RATE IS MEASURED, NOT DEFAULTED — same fixture, three cadences. */
	CHECK(reac_tap_survey_rate(&s) == 48000, "4000 pps reads 48 kHz");

	static const struct { double pps; int hz; const char *msg; } rates[] = {
		{ 8000.0, 96000, "8000 pps reads 96 kHz (the control that 48000 is not a default)" },
		{ 3675.0, 44100, "3675 pps reads 44.1 kHz" },
	};
	for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; i++) {
		snprintf(path, sizeof path, "%s/mirror%d.pcap", dir, rates[i].hz);
		write_mirror_pcap(path, rates[i].pps);
		struct reac_tap_survey r;
		reac_tap_survey_init(&r);
		survey_file(path, &r, 100000);
		CHECK(reac_tap_survey_rate(&r) == rates[i].hz, rates[i].msg);
	}

	/* An empty roster answers 0, and 0 is an absence — never 48000. */
	struct reac_tap_survey empty;
	reac_tap_survey_init(&empty);
	CHECK(reac_tap_survey_rate(&empty) == 0, "no master heard: the rate is absent, not defaulted");
	uint8_t junk[64] = { 0 };
	CHECK(reac_tap_survey_frame(&empty, junk, sizeof junk, 1) == REAC_TAP_NOT_REAC,
	      "a non-0x8819 frame is refused with a reason");
	CHECK(empty.n == 0, "and mints no stream");
}

static const uint8_t SELF_MAC[6] = { 0x34, 0x5a, 0x60, 0x9f, 0x9e, 0xbe }; /* this host's NIC */

#define CORPUS_RECORDS 60000u

static void dump(const char *what, const struct reac_tap_survey *s)
{
	for (unsigned i = 0; i < s->n; i++) {
		const struct reac_tap_stream *t = &s->stream[i];
		printf("  %s stream %u: %s %02x:%02x:%02x:%02x:%02x:%02x %u ch "
		       "frames=%llu dups=%llu gaps=%llu model=%d\n", what, i,
		       t->kind == REAC_TAP_STREAM_MASTER ? "master" : "box   ",
		       t->src[0], t->src[1], t->src[2], t->src[3], t->src[4], t->src[5],
		       t->channels, (unsigned long long)t->frames,
		       (unsigned long long)t->dups, (unsigned long long)t->gaps,
		       t->model_index);
	}
}

static void arm_corpus(const char *path)
{
	/* (a) THE SEGMENT AS RECORDED, with the self-filter on. */
	struct reac_tap_survey s;
	reac_tap_survey_init(&s);
	reac_tap_survey_set_self(&s, SELF_MAC);
	survey_file(path, &s, CORPUS_RECORDS);
	dump("corpus", &s);
	printf("  corpus rate: %d Hz, self-echo frames refused: %llu\n",
	       reac_tap_survey_rate(&s), (unsigned long long)s.frames_self);

	const struct reac_tap_stream *m = reac_tap_survey_master(&s);
	const struct reac_tap_stream *b = reac_tap_survey_box(&s, BOX_MAC);
	CHECK(m != NULL, "corpus: the M-200's downstream is heard as the master");
	CHECK(b != NULL, "corpus: the S-1608 at 00:40:ab:c4:80:3b is heard as a box");
	if (m) {
		CHECK(m->channels == 40, "corpus: the master's downstream is 40 channels");
		CHECK(memcmp(m->src, MASTER_MAC, 6) == 0, "corpus: the master is 00:40:ab:c9:cc:03");
		CHECK(m->frames > 19000, "corpus: the master stream carries its frames");
	}
	if (b)
		CHECK(b->channels == BOX_CH, "corpus: the S-1608 return is 16 channels");
	CHECK(reac_tap_survey_rate(&s) == 48000, "corpus: the master's cadence reads 48 kHz");
	CHECK(s.n == 2, "corpus: with the self-filter on, exactly the desk and the box");
	CHECK(s.frames_self > 0, "corpus: the self-filter HAD something to catch");

	/* (b) THE SELF-FILTER'S CONTROL: the same file, filter off. Our own slave's frames
	 * mint a third stream. Without this arm the filter above could be a no-op. */
	struct reac_tap_survey unfiltered;
	reac_tap_survey_init(&unfiltered);
	survey_file(path, &unfiltered, CORPUS_RECORDS);
	dump("unfiltered", &unfiltered);
	CHECK(unfiltered.n == 3, "corpus: unfiltered, this host's own slave is a third stream");
	CHECK(reac_tap_survey_box(&unfiltered, SELF_MAC) != NULL,
	      "corpus: and that third stream is ours (34:5a:60:9f:9e:be)");

	/* (c) THE MIRROR TRANSFORM over the same real frames: every copy is a duplicate,
	 * and the master's one real gap is still exactly one. */
	struct reac_tap_survey mirrored;
	reac_tap_survey_init(&mirrored);
	reac_tap_survey_set_self(&mirrored, SELF_MAC);
	survey_file_mirrored(path, &mirrored, CORPUS_RECORDS, 1);
	dump("mirrored", &mirrored);
	const struct reac_tap_stream *mm = reac_tap_survey_master(&mirrored);
	const struct reac_tap_stream *mb = reac_tap_survey_box(&mirrored, BOX_MAC);
	CHECK(mm && m && mm->frames == m->frames,
	      "corpus+mirror: the master's accepted frame count is unchanged by the twin");
	CHECK(mm && m && mm->dups == m->frames,
	      "corpus+mirror: every twin of a real frame lands in dups");
	CHECK(mm && m && mm->gaps == m->gaps,
	      "corpus+mirror: the twin adds NOT ONE gap — the real hole is still the only one");
	CHECK(mb && b && mb->frames == b->frames && mb->dups == b->frames,
	      "corpus+mirror: the box stream collapses its twin the same way");
	CHECK(mirrored.n == 2, "corpus+mirror: still exactly two streams");
}

/* HOLD WITH A DECLARED LIMIT, in the tap. A broadcast stream is UNRESOLVED until its
 * source proves what it is, for at most ONE MASTER-ONLY CADENCE IN FRAMES AT ITS OWN PACE
 * (reac-protocol's master_cadence group, through reac_arbitration.h); then a source with
 * no master-only op is a box. Frames are fed at their real pace, counters consecutive. */
static void feed(struct reac_tap_survey *s, const uint8_t *mac, size_t len, int fps,
                 uint32_t n, uint16_t *ctr, uint64_t *ts, int announce_last)
{
	uint8_t f[REAC_FRAME_BYTES];
	for (uint32_t k = 0; k < n; k++) {
		mk_frame(f, len, BCAST, mac, (*ctr)++);
		memset(f + 16, 0, 34);                       /* a FILLER: type 0000, zero block */
		if (announce_last && k + 1 == n)
			memcpy(f + 16, FX_ANNOUNCE, sizeof FX_ANNOUNCE);
		reac_tap_survey_frame(s, f, len, *ts);
		*ts += 1000000ull / (uint64_t)fps;
	}
}

static void arm_hold(void)
{
	static const uint8_t DESK[6]  = { 0x00, 0x40, 0xab, 0xc9, 0x91, 0x9c };
	static const uint8_t FLOOD[6] = { 0x00, 0x40, 0xab, 0x16, 0x08, 0x02 };
	static const uint8_t WIDE[6]  = { 0x00, 0x40, 0xab, 0x40, 0x40, 0x02 };
	static const uint8_t FAST[6]  = { 0x00, 0x40, 0xab, 0x96, 0x96, 0x02 };
	const int fps48 = 4000, fps96 = 8000;
	const uint32_t W48 = reac_master_only_cadence_frames(fps48);   /* 4000 */
	const uint32_t W96 = reac_master_only_cadence_frames(fps96);   /* 8000 */
	struct reac_tap_survey s;
	reac_tap_survey_init(&s);

	/* A desk's downstream: held for most of a cadence, then its master op inside it. */
	uint16_t c = 1;
	uint64_t ts = 5000000ull;
	feed(&s, DESK, REAC_FRAME_BYTES, fps48, W48 / 2, &c, &ts, 0);
	CHECK(s.n == 1 && s.stream[0].kind == REAC_TAP_STREAM_UNRESOLVED,
	      "hold: a desk's downstream before its master op is UNRESOLVED, not the master");
	CHECK(reac_tap_survey_master(&s) == NULL, "hold: and no master is claimed yet");
	feed(&s, DESK, REAC_FRAME_BYTES, fps48, 1, &c, &ts, 1);
	CHECK(s.stream[0].kind == REAC_TAP_STREAM_MASTER,
	      "hold: a desk whose master op arrives inside the window is the MASTER stream");

	/* Flood-only boxes at 48 kHz, 16 and 40 wide: held for exactly one cadence of frames,
	 * a box on the next. */
	const struct { const uint8_t *mac; int w; } boxes[] = { { FLOOD, 16 }, { WIDE, 40 } };
	for (unsigned b = 0; b < 2; b++) {
		const size_t len = REAC_UPSTREAM_OVERHEAD + (size_t)boxes[b].w * REAC_UPSTREAM_BYTES_PER_CH;
		uint16_t bc = 100;
		uint64_t bts = 5000000ull;
		feed(&s, boxes[b].mac, len, fps48, W48, &bc, &bts, 0);
		const struct reac_tap_stream *st = &s.stream[s.n - 1];
		CHECK(st->kind == REAC_TAP_STREAM_UNRESOLVED && st->channels == (unsigned)boxes[b].w,
		      "hold: a flood-only broadcast is UNRESOLVED for one cadence of frames");
		feed(&s, boxes[b].mac, len, fps48, 1, &bc, &bts, 0);
		CHECK(st->kind == REAC_TAP_STREAM_BOX,
		      "hold: a flood-only broadcast is a BOX past one cadence (16 and 40 wide)");
	}

	/* IN FRAMES AT THE CURRENT RATE: a 96 kHz flood has advanced 48 kHz's window and is
	 * still held, because at its own pace the cadence is 8000 frames. */
	{
		uint16_t fc = 7;
		uint64_t fts = 5000000ull;
		feed(&s, FAST, BOX_LEN, fps96, W48 + 1, &fc, &fts, 0);
		const struct reac_tap_stream *st = &s.stream[s.n - 1];
		CHECK(st->kind == REAC_TAP_STREAM_UNRESOLVED,
		      "hold: at 96 kHz, 4001 frames is half a cadence — still held");
		feed(&s, FAST, BOX_LEN, fps96, W96 - W48, &fc, &fts, 0);
		CHECK(st->kind == REAC_TAP_STREAM_BOX, "hold: at 96 kHz, a box past 8000 frames");
	}

	/* The limit also applies at the END of a survey, with no further frame. */
	struct reac_tap_survey s2;
	reac_tap_survey_init(&s2);
	uint16_t c2 = 9;
	uint64_t ts2 = 5000000ull;
	feed(&s2, FLOOD, BOX_LEN, fps48, 10, &c2, &ts2, 0);
	const uint64_t first = 5000000ull, W_US = reac_master_only_cadence_ns(fps48) / 1000ull;
	CHECK(reac_tap_survey_resolve(&s2, first + W_US - 1) == 1, "hold: still held just inside");
	CHECK(reac_tap_survey_resolve(&s2, first + W_US) == 0 &&
	      s2.stream[0].kind == REAC_TAP_STREAM_BOX, "hold: a box at the window's end");
}

int main(int argc, char **argv)
{
	char dir[] = "/tmp/reac_tap_test_XXXXXX";
	if (!mkdtemp(dir)) {
		printf("FAIL: mkdtemp\n");
		return 1;
	}
	arm_synthetic(dir);
	arm_hold();

	/* The segment answers publish the KIND they are given, never one inferred from a
	 * width: a joined box on M that is 40 wide reads `box`, a refused box `box` with its
	 * refusal, a joined desk `desk` (operator ruling 2026-09-25). */
	{
		struct reac_segment_answer a;
		reac_segment_answer_slave_kind(&a, 1, 0x0040abc4063bull, 48000, REAC_RIVAL_BOX);
		CHECK(strcmp(a.rival_kind, "box") == 0 && strcmp(a.refusal, "none") == 0,
		      "segment: a joined box on M reads box, whatever its width");
		reac_segment_answer_slave_kind(&a, 1, 0x0040abc9919cull, 48000, REAC_RIVAL_DESK);
		CHECK(strcmp(a.rival_kind, "desk") == 0, "segment: a joined desk reads desk");
		reac_segment_answer_slave_kind(&a, 0, 0, 0, REAC_RIVAL_DESK);
		CHECK(strcmp(a.rival_kind, "none") == 0 && strcmp(a.master_state, "none") == 0,
		      "segment: nothing heard is none, whatever kind was passed");
		reac_segment_answer_refused_kind(&a, REAC_RIVAL_BOX, 0x0040abc4063bull, 48000);
		CHECK(strcmp(a.rival_kind, "box") == 0 && strcmp(a.refusal, "rival-master-box") == 0,
		      "segment: a refused box carries its refusal code");
	}

	if (argc > 1 && access(argv[1], R_OK) == 0) {
		printf("test_tap: corpus arm on %s\n", argv[1]);
		arm_corpus(argv[1]);
	} else {
		/* Visible, never silent: the corpus arm is a different claim from the
		 * synthetic one and its absence must not read as a pass. */
		printf("test_tap: CORPUS ARM NOT RUN (no readable capture given as argv[1]) — "
		       "the synthetic mirror arm alone ran\n");
	}

	char cmd[600];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
	if (system(cmd) != 0)
		printf("note: could not clean %s\n", dir);

	printf(fails ? "test_tap: %d FAILURES\n" : "test_tap: OK (%d failures)\n", fails);
	return fails ? 1 : 0;
}
