// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* THE EMISSION LAW, ASSERTED AGAINST REAL TRAFFIC: A MASTER EMITS EACH
 * DOWNSTREAM FRAME EXACTLY ONCE.
 *
 * WHY THIS FILE EXISTS. On 2026-08-20 a branch proposed the opposite — that a
 * REAC master emits every downstream frame twice, back-to-back, same bytes and
 * same counter (reac-pw #92, `REAC_PACER_TX_REPS 2`) — and it was believed
 * through two triage passes. The claim came from reading two MIRRORED captures
 * raw. A tap that mirrors both directions of a port hands the capture each
 * transiting frame twice: one copy clean, one carrying the two bytes of the
 * frame's own Ethernet FCS (<reac/reac.h>, libreac 0b65341; reac-tools dropped
 * that twin in `dedupe_mirror_twins()`, PR #4 / 33a9fbd, three weeks earlier;
 * the corpus itself was stripped to one copy per frame in reac-captures
 * 8bd985e). Every one of those facts lived in prose or in a tool nobody was
 * obliged to run, so none of them stopped the branch.
 *
 * A fact the product relies on and no test asserts is a fact the next branch
 * may overwrite. This is that assertion, and it runs in `make test` with no
 * corpus, no capture and no network on the machine: the wire records come from
 * tests/wire-invariants.inc (tools/gen-wire-invariants.py, real traffic).
 *
 * WHAT IS ASSERTED
 *
 *  1. ONE EMISSION PER SLOT. After the mirror twin is dropped, NO source ever
 *     repeats a counter: a clean-geometry frame is never followed by a second
 *     clean-geometry frame carrying the same counter. Masters and boxes alike,
 *     in mirrored captures and in a non-mirrored one.
 *
 *  2. THE PACKET RATE IS THE SAMPLE RATE. The counter advances at pps =
 *     rate / 12 (reac-protocol wire-format.md; REAC_MODE_*), so the measured
 *     advance snapped by the library's own reac_rate_snap() must equal the rate
 *     the capture was taken at. A doubled emission does not change this — which
 *     is exactly why #92's "8000 pps at 48 kHz" was the tap and not the desk.
 *
 *  3. THE LINK BOUND. REAC runs on 100BASE-TX. 96 kHz already costs
 *     8000 x (1492 + 24) x 8 = 97.0 Mbit/s of it, so a master emitting each
 *     frame twice would need 194 Mbit/s — a behaviour that does not fit on the
 *     wire is not a behaviour the protocol can require.
 *
 *  4. THE RESIDUE IS THE TAP, NOT THE WIRE. Residue-length frames (a clean
 *     52+36n plus two bytes) appear ONLY in mirrored captures, never in a
 *     capture taken off a plain NIC.
 *
 * WHY IT CANNOT PASS VACUOUSLY (this file's own failure mode, and the one that
 * produced #92 in the first place — a probe that reports absence without ever
 * having been able to report presence):
 *
 *  a. Every arm reports how many frames it examined, and a capture that yields
 *     no frames, or fewer than the fixture declares, FAILS. An empty scan is a
 *     broken scan, never a clean result.
 *  b. POSITIVE CONTROL: the mirrored captures MUST show twins. A dedup that
 *     stopped recognising them would make assertion 1 trivially true.
 *  c. NEGATIVE CONTROL (the sabotage, permanently wired in): the same checker
 *     is run over a SYNTHETICALLY DOUBLED stream — every clean record cloned
 *     with its own counter, which is precisely what #92 proposed to emit — and
 *     it MUST report violations. This is what keeps the dedup honest: a twin is
 *     a CLEAN copy paired with a RESIDUE copy, and two clean copies of one
 *     counter are never a twin, they are a second emission.
 *
 * Pure: no socket, no thread, no file, no rig.
 */

#include "reac/reac.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
	printf("FAIL (line %d): ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

#include "wire-invariants.inc"

/* A clean REAC frame length is 52 + 36n in both directions (reac.h's geometry;
 * reac-protocol spec/reac.ksy derives both widths from it). */
static int is_clean_len(unsigned len)
{
	return len >= REAC_UPSTREAM_OVERHEAD &&
	       (len - REAC_UPSTREAM_OVERHEAD) % REAC_UPSTREAM_BYTES_PER_CH == 0;
}

/* THE MIRROR TWIN, defined exactly as ingest defines it (reac.h:35-47, and
 * reac-tools' dedupe_mirror_twins, 33a9fbd): the same source, the same counter,
 * the same frame, inside the twin window, where ONE copy is clean and the other
 * is that clean length PLUS the two bytes of FCS residue.
 *
 * THE RESIDUE IS LOAD-BEARING. Dropping any same-counter repeat would make this
 * whole file vacuous: it would erase a genuine double emission along with the
 * tap's copy. Two CLEAN copies of one counter are not a twin, they are a second
 * emission, and that is what this file exists to refuse. */
#define TWIN_US   2000u    /* a mirrored copy arrives microseconds behind */
#define SLOT_US   5000u    /* beyond this the corpus distillation may have sampled */

struct verdict {
	unsigned examined;      /* records walked                              */
	unsigned kept;          /* emissions left after the twin is dropped    */
	unsigned twins;         /* mirror copies dropped                       */
	unsigned violations;    /* a counter emitted more than once            */
	unsigned residue;       /* records whose length is a clean length + 2  */
	unsigned clean;         /* records of clean geometry                   */
	uint32_t advance[8];    /* counter advance per source                  */
	uint32_t span_us[8];    /* first-to-last time per source               */
	unsigned frames[8];     /* kept emissions per source                   */
	unsigned len[8];        /* the geometry each source emits              */
};

/* Walk one capture the way ingest would, in BURSTS: the maximal run of records
 * from one source carrying one counter, inside the slot window. The law is
 * about the burst, not about adjacent pairs — a pair rule is defeated by the
 * order the tap happens to deliver the two copies in (measured: with the
 * residue copy first, a pairwise rule swallows a genuine double emission as a
 * twin, and the negative control below caught exactly that).
 *
 * In one burst the wire allows AT MOST ONE clean emission, and the tap adds at
 * most one copy of it. So:
 *   - clean copies beyond the first   -> a second emission: A VIOLATION
 *   - residue copies beyond the first -> a second emission seen by the tap: a
 *                                        violation too
 *   - one residue copy beside a clean one -> the mirror twin, dropped
 * Nothing here knows which capture it is looking at. */
struct burst {
	int open;
	uint16_t counter;
	unsigned clean, residue, len;
	uint32_t t0, tlast;
	uint16_t prev_counter;         /* the previous CLOSED burst's counter */
	uint32_t prev_t;
	int have_prev;
};

/* Close one source's burst into the verdict. */
static void burst_close(struct burst *b, unsigned s, struct verdict *v)
{
	if (!b->open)
		return;
	if (b->clean > 1)
		v->violations += b->clean - 1;       /* a SECOND EMISSION */
	if (b->residue > 1)
		v->violations += b->residue - 1;     /* seen twice by the tap too */
	if (b->clean && b->residue && b->tlast - b->t0 <= TWIN_US)
		v->twins += b->residue;              /* the tap's copy, not the wire's */
	if (b->have_prev) {
		v->advance[s] += (uint32_t)reac_counter_gap(b->prev_counter, b->counter) + 1;
		if (b->t0 >= b->prev_t)
			v->span_us[s] += b->t0 - b->prev_t;
	}
	if (b->len)
		v->len[s] = b->len;
	v->frames[s]++;
	v->kept++;
	b->prev_counter = b->counter;
	b->prev_t = b->t0;
	b->have_prev = 1;
	b->open = 0;
}

static void judge(const struct wi_record *r, unsigned n, struct verdict *v)
{
	memset(v, 0, sizeof *v);
	struct burst b[8];
	memset(b, 0, sizeof b);
	for (unsigned i = 0; i < n; i++) {
		unsigned s = r[i].src < 8 ? r[i].src : 7;
		v->examined++;
		if (b[s].open && (r[i].counter != b[s].counter ||
		                  r[i].rel_us - b[s].t0 > SLOT_US))
			burst_close(&b[s], s, v);
		if (!b[s].open) {
			b[s].open = 1;
			b[s].counter = r[i].counter;
			b[s].clean = b[s].residue = b[s].len = 0;
			b[s].t0 = r[i].rel_us;
		}
		b[s].tlast = r[i].rel_us;
		if (is_clean_len(r[i].origlen)) {
			b[s].clean++;
			b[s].len = r[i].origlen;
			v->clean++;
		} else if (is_clean_len(r[i].origlen - 2)) {
			b[s].residue++;
			v->residue++;
		}
	}
	for (unsigned s = 0; s < 8; s++)
		burst_close(&b[s], s, v);
}

int main(void)
{
	const unsigned ncap = (unsigned)(sizeof WI_CAPTURES / sizeof WI_CAPTURES[0]);
	CHECK(ncap >= 3, "the fixture carries %u captures, expected at least 3", ncap);

	unsigned total_examined = 0, total_twins = 0, mirrored_seen = 0;

	for (unsigned c = 0; c < ncap; c++) {
		const struct wi_capture *cap = &WI_CAPTURES[c];
		struct verdict v;

		/* (a) an empty or short fixture is a broken scan, not a clean one */
		CHECK(cap->n >= 500, "%s: only %u records in the fixture", cap->name, cap->n);
		judge(cap->rec, cap->n, &v);
		CHECK(v.examined == cap->n, "%s: examined %u of %u records",
		      cap->name, v.examined, cap->n);
		CHECK(v.clean > 400, "%s: only %u clean-geometry frames examined",
		      cap->name, v.clean);
		total_examined += v.examined;
		total_twins += v.twins;

		int mirror = strcmp(cap->tap, "mirror") == 0;

		/* (1) ONE EMISSION PER SLOT */
		CHECK(v.violations == 0,
		      "%s: %u clean frames repeat a clean counter -- a master or box "
		      "emitted the same frame twice", cap->name, v.violations);

		/* (b) the probe can see a duplicate where one exists, and (4) the
		 *     residue belongs to the tap and to nothing else */
		if (mirror) {
			mirrored_seen++;
			CHECK(v.twins > 100, "%s: mirrored capture yielded only %u twins -- "
			      "the twin criterion has stopped matching", cap->name, v.twins);
			CHECK(v.residue > 100, "%s: mirrored capture carries only %u "
			      "residue-length frames", cap->name, v.residue);
		} else {
			CHECK(v.twins == 0, "%s: %u mirror twins in a capture taken off a "
			      "plain NIC", cap->name, v.twins);
			CHECK(v.residue == 0, "%s: %u residue-length frames in a capture "
			      "taken off a plain NIC -- the +2 is not a wire variant",
			      cap->name, v.residue);
		}

		/* (2) the packet rate IS the sample rate, and (3) it fits the link */
		for (unsigned s = 0; s < 8; s++) {
			if (v.frames[s] < 200 || v.span_us[s] < 200000u || !v.len[s])
				continue;
			double pps = (double)v.advance[s] * 1e6 / (double)v.span_us[s];
			int snapped = reac_rate_snap(pps);
			const struct reac_mode *m = reac_mode_for(cap->rate);
			double bits = pps * (double)(v.len[s] + 24) * 8.0;
			printf("  %s [%s]: %u frames, %.0f pps, snaps to %d Hz, %.1f Mbit/s\n",
			       cap->name, cap->macs[s < cap->n_macs ? s : 0],
			       v.frames[s], pps, snapped, bits / 1e6);
			CHECK(snapped == cap->rate,
			      "%s: source %u advances %.0f counters/s, which snaps to %d Hz, "
			      "not the %d Hz the capture was taken at",
			      cap->name, s, pps, snapped, cap->rate);
			CHECK((double)m->sample_rate / m->samples_per_pkt > pps * 0.85 &&
			      (double)m->sample_rate / m->samples_per_pkt < pps * 1.15,
			      "%s: source %u paces %.0f pps, more than 15%% off the %d/%d "
			      "the mode declares", cap->name, s, pps,
			      m->sample_rate, m->samples_per_pkt);
			CHECK(bits <= 100e6,
			      "%s: source %u would need %.1f Mbit/s, more than 100BASE-TX "
			      "carries", cap->name, s, bits / 1e6);
		}
	}

	CHECK(mirrored_seen >= 2, "only %u mirrored captures in the fixture -- the "
	      "positive control needs one", mirrored_seen);
	CHECK(total_twins > 200, "the whole fixture yielded %u twins", total_twins);

	/* (3) THE LINK BOUND, as arithmetic: why a doubled 96 kHz master cannot
	 *     exist even in principle. 8000 pps of 1492 B frames is already 97 % of
	 *     100BASE-TX; twice that does not fit. */
	double once = 8000.0 * (REAC_FRAME_BYTES + 24) * 8.0;
	CHECK(once < 100e6, "a single-emission 96 kHz master needs %.1f Mbit/s", once / 1e6);
	CHECK(2.0 * once > 100e6,
	      "a doubled 96 kHz master would need %.1f Mbit/s -- if that fits, this "
	      "argument no longer holds", 2.0 * once / 1e6);

	/* (c) THE NEGATIVE CONTROL / permanent sabotage: double the wire the way
	 *     #92 proposed and require the checker to catch it. If this arm ever
	 *     goes quiet, assertion 1 above has become decoration. */
	{
		const struct wi_capture *cap = &WI_CAPTURES[0];
		static struct wi_record doubled[4096];
		unsigned n = 0;
		for (unsigned i = 0; i < cap->n && n + 2 < 4096; i++) {
			if (!is_clean_len(cap->rec[i].origlen)) {
				doubled[n++] = cap->rec[i];
				continue;
			}
			doubled[n++] = cap->rec[i];
			doubled[n] = cap->rec[i];       /* same bytes, same counter */
			doubled[n].rel_us = cap->rec[i].rel_us + 8;
			n++;
		}
		struct verdict v;
		judge(doubled, n, &v);
		CHECK(n > cap->n, "the doubled stream is not longer than the original");
		CHECK(v.violations > 400,
		      "a stream where every clean frame is emitted twice produced only %u "
		      "violations -- the emission law is not being enforced", v.violations);
		printf("  negative control: %u records doubled -> %u violations caught\n",
		       n, v.violations);
	}

	/* (a) an empty input is refused rather than passed */
	{
		struct verdict v;
		judge(WI_CAPTURES[0].rec, 0, &v);
		CHECK(v.examined == 0 && v.kept == 0, "the empty walk invented records");
		printf("  empty input: examined=%u (a scan that sees nothing proves nothing)\n",
		       v.examined);
	}

	if (fails) {
		printf("FAILED: %d wire-invariant checks\n", fails);
		return 1;
	}
	printf("OK: wire invariants over %u real records from %u captures -- one "
	       "emission per slot, %u mirror twins dropped, pace = rate, within the "
	       "100BASE-TX bound\n", total_examined, ncap, total_twins);
	return 0;
}
