// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* pace_hist — what did the CADENCE actually look like on the wire?
 *
 * The pacer reports its own health (late wakes, repaid slot debt, worst single
 * debt, transmit deficit) and every one of those numbers is the pacer's opinion
 * of itself. Comparing a userspace pacer against a kernel one on those numbers
 * alone compares two self-reports, in two different implementations, that do not
 * even count in the same place. This tool is the EXTERNAL TRUTH for that
 * comparison: a capture taken on a mirror port, off the machine under test, read
 * back as the distribution of inter-frame intervals per talker. Same metric,
 * same instrument, both arms.
 *
 * What it measures, per source MAC:
 *   frames, span, and the packet rate as a RATIO over the span (never a nominal
 *   restated); the interval mean / min / max / stddev; p50, p90, p99, p99.9;
 *   how many intervals ran long by 1.5x, 2x and 4x the nominal period (a LATE
 *   slot on the wire, whatever the sender calls it internally) and how many ran
 *   short by half (a CATCH-UP landing, the repayment of a late one).
 *
 * TIMESTAMP RESOLUTION IS 1 us AND THAT IS A REAL BOUND. Classic pcap carries
 * microseconds, so at 8000 fps (125 us nominal) this instrument resolves jitter
 * to about 0.8% of a period and CANNOT see anything finer. It is pointed at a
 * signal far above that: the userspace pacer's measured single-miss debt runs
 * 250 us at p50 and 2000 us at its worst, i.e. 250x to 2000x the quantum. A
 * report of "no late slots" from this tool means no MILLISECOND-scale miss, and
 * says nothing about sub-microsecond phase. Do not quote it for the latter.
 * (A nanosecond-magic pcap is REFUSED by name below rather than read wrong.)
 *
 * ITS OWN CONTROL. `--self-test` drives the same accumulator with synthetic
 * interval series whose answers are closed-form: a perfectly regular series must
 * report zero late slots, and the same series with three 1 ms stalls injected
 * must report exactly three at 4x. An instrument that has not been shown to
 * detect the presence cannot be believed about the absence, and "the kernel
 * pacer had no late slots" is exactly an absence claim. Run --self-test first;
 * tools/pace-compare.sh does.
 *
 * It reads a file. It opens no socket and transmits nothing.
 */

#include <reac/reac.h>
#include <reac/pcap_source.h>

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Intervals are bucketed at 1 us because the timestamps ARE 1 us -- a finer
 * bucket would invent resolution the capture does not carry. 8 ms of range
 * covers every plausible stall; anything longer lands in the overflow bucket and
 * is still exact in min/max/sum, so a percentile can be read as ">= 8192 us"
 * rather than silently clamped. */
#define IV_BUCKETS 8192u

struct pace_acc {
	uint64_t n;                 /* intervals accumulated                     */
	uint64_t sum_ns;            /* exact, for the mean                       */
	long double sum_sq_ns;      /* exact enough for a stddev over 1e6 samples */
	uint64_t min_ns, max_ns;
	uint64_t bucket[IV_BUCKETS + 1];   /* [IV_BUCKETS] = >= 8192 us          */
	uint64_t late_1p5, late_2x, late_4x, catchup;
	uint64_t nominal_ns;        /* 0 until set: the period the arm was asked for */
};

static void acc_init(struct pace_acc *a, uint64_t nominal_ns)
{
	memset(a, 0, sizeof(*a));
	a->min_ns = UINT64_MAX;
	a->nominal_ns = nominal_ns;
}

/* Fold ONE inter-frame interval. Pure: the pcap path and --self-test both enter
 * here and nowhere else, so the control exercises the measured code. */
static void acc_add(struct pace_acc *a, uint64_t iv_ns)
{
	uint64_t us = iv_ns / 1000u;

	a->n++;
	a->sum_ns += iv_ns;
	a->sum_sq_ns += (long double)iv_ns * (long double)iv_ns;
	if (iv_ns < a->min_ns)
		a->min_ns = iv_ns;
	if (iv_ns > a->max_ns)
		a->max_ns = iv_ns;
	a->bucket[us < IV_BUCKETS ? us : IV_BUCKETS]++;

	if (a->nominal_ns) {
		if (iv_ns * 2 >= a->nominal_ns * 3) a->late_1p5++;
		if (iv_ns >= a->nominal_ns * 2)     a->late_2x++;
		if (iv_ns >= a->nominal_ns * 4)     a->late_4x++;
		if (iv_ns * 2 <= a->nominal_ns)     a->catchup++;
	}
}

/* Percentile from the histogram, in us. Returns the bucket's LOWER edge, which
 * is the honest answer at 1 us resolution. The overflow bucket answers 8192. */
static double acc_pct(const struct pace_acc *a, double p)
{
	uint64_t want = (uint64_t)((double)a->n * p);
	uint64_t seen = 0;

	if (!a->n)
		return 0.0;
	if (want >= a->n)
		want = a->n - 1;
	for (uint32_t i = 0; i <= IV_BUCKETS; i++) {
		seen += a->bucket[i];
		if (seen > want)
			return (double)i;
	}
	return (double)IV_BUCKETS;
}

static double acc_mean_us(const struct pace_acc *a)
{
	return a->n ? (double)a->sum_ns / (double)a->n / 1000.0 : 0.0;
}

static double acc_sd_us(const struct pace_acc *a)
{
	long double m, var;

	if (a->n < 2)
		return 0.0;
	m = (long double)a->sum_ns / (long double)a->n;
	var = a->sum_sq_ns / (long double)a->n - m * m;
	if (var < 0)
		var = 0;
	return (double)(sqrtl(var) / 1000.0L);
}

/* ---- the pcap arm -------------------------------------------------------- */

#define NT 8
struct talker {
	uint8_t mac[6];
	int seen;
	uint64_t first_ns, last_ns;
	uint64_t frames;
	struct pace_acc acc;
};

static struct talker tk[NT];
static int ntk;

static struct talker *talker_for(const uint8_t *mac, uint64_t nominal_ns)
{
	for (int i = 0; i < ntk; i++)
		if (!memcmp(tk[i].mac, mac, 6))
			return &tk[i];
	if (ntk == NT)
		return NULL;
	memcpy(tk[ntk].mac, mac, 6);
	acc_init(&tk[ntk].acc, nominal_ns);
	return &tk[ntk++];
}

/* reac_rate_snap has no floor: it is a NEAREST-of-three and will answer 44100 Hz
 * for 196 pps, which is not a cadence at all but a partial capture. A rate
 * printed for a packet rate that is nowhere near a legal pace is a false signal
 * of exactly the shape this whole tool exists to avoid, so the snap is gated on
 * the observed rate being within 10% of one of the three legal paces (the same
 * closed list reac_rate_snap maps, 3675 / 4000 / 8000). Outside that band the
 * answer is "not a pace", and the caller is told what it measured instead. */
static int pace_is_legal(double pps)
{
	static const double legal[3] = { 3675.0, 4000.0, 8000.0 };

	for (int i = 0; i < 3; i++)
		if (pps >= legal[i] * 0.9 && pps <= legal[i] * 1.1)
			return 1;
	return 0;
}

static void print_mac(const uint8_t *m)
{
	printf("%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* A nanosecond-precision pcap has its own magic and DIFFERENT per-record units.
 * pcap_source refuses it (correctly) with the same -1 a missing file gives, and
 * "cannot open" sends the reader hunting for a path bug. Name it instead. */
static int magic_is_nanosecond(const char *path)
{
	uint8_t m[4];
	FILE *f = fopen(path, "rb");
	int nano = 0;

	if (!f)
		return 0;
	if (fread(m, 1, 4, f) == 4) {
		uint32_t v = (uint32_t)m[0] | ((uint32_t)m[1] << 8) |
		             ((uint32_t)m[2] << 16) | ((uint32_t)m[3] << 24);
		nano = (v == 0xA1B23C4Du || v == 0x4D3CB2A1u);
	}
	fclose(f);
	return nano;
}

static int self_test(void);

int main(int argc, char **argv)
{
	const char *path = NULL;
	const uint8_t *only = NULL;
	uint8_t only_mac[6];
	int fps = 0, want_hist = 0;
	uint64_t min_frames = 1000;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--self-test"))
			return self_test();
		else if (!strcmp(argv[i], "--fps") && i + 1 < argc)
			fps = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--min-frames") && i + 1 < argc)
			min_frames = strtoull(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--hist"))
			want_hist = 1;
		else if (!strcmp(argv[i], "--src") && i + 1 < argc) {
			unsigned b[6];
			if (sscanf(argv[++i], "%x:%x:%x:%x:%x:%x",
			           &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
				fprintf(stderr, "pace_hist: --src wants aa:bb:cc:dd:ee:ff\n");
				return 2;
			}
			for (int k = 0; k < 6; k++)
				only_mac[k] = (uint8_t)b[k];
			only = only_mac;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "pace_hist: unknown option %s\n", argv[i]);
			return 2;
		} else
			path = argv[i];
	}

	if (!path) {
		fprintf(stderr,
		        "usage: pace_hist [--fps N] [--src MAC] [--min-frames N] [--hist] FILE.pcap\n"
		        "       pace_hist --self-test\n"
		        "  --fps is the cadence the arm was ASKED for (8000 @96k, 4000 @48k,\n"
		        "        3675 @44.1k); without it no late/catch-up verdict is given,\n"
		        "        because those are all relative to a nominal period.\n");
		return 2;
	}

	if (magic_is_nanosecond(path)) {
		fprintf(stderr,
		        "pace_hist: %s is a NANOSECOND-precision pcap (magic 0xA1B23C4D).\n"
		        "  The reader is classic-pcap only and would misread every timestamp\n"
		        "  by 1000x. Re-capture without tcpdump's nanosecond flag.\n", path);
		return 1;
	}

	uint64_t nominal_ns = fps > 0 ? (uint64_t)(1000000000.0 / (double)fps + 0.5) : 0;

	struct pcap_source ps;
	if (pcap_source_open(&ps, path) != 0) {
		fprintf(stderr, "pace_hist: cannot open %s as a classic pcap\n", path);
		return 1;
	}

	static uint8_t buf[4096];
	uint64_t truncated = 0;
	for (;;) {
		uint64_t ts_us = 0;
		long n = pcap_source_next(&ps, buf, sizeof buf, &ts_us);
		if (n <= 0)
			break;
		if (!reac_frame_is_reac(buf, (size_t)n) || (size_t)n < 16)
			continue;
		if (ps.last_orig_len > (uint32_t)n)
			truncated++;   /* a snaplen capture: timing is still exact */
		if (only && memcmp(buf + 6, only, 6))
			continue;

		struct talker *k = talker_for(buf + 6, nominal_ns);
		if (!k)
			continue;
		uint64_t ns = ts_us * 1000ull;
		k->frames++;
		if (k->seen) {
			/* A capture is in file order, which is arrival order. A
			 * non-monotonic timestamp is a broken capture, not a
			 * negative interval -- count it, never fold it. */
			if (ns >= k->last_ns)
				acc_add(&k->acc, ns - k->last_ns);
		} else
			k->first_ns = ns;
		k->seen = 1;
		k->last_ns = ns;
	}
	pcap_source_close(&ps);

	if (!ntk) {
		fprintf(stderr, "pace_hist: no REAC talker in %s -- an empty scan is not a result\n",
		        path);
		return 1;
	}

	printf("# pace_hist: resolution 1 us (classic pcap); nominal %s\n",
	       fps > 0 ? "given" : "NOT GIVEN -- no late/catch-up verdict");
	if (truncated)
		printf("# %" PRIu64 " records were snaplen-truncated; timing unaffected\n", truncated);

	int rc = 0;
	for (int i = 0; i < ntk; i++) {
		struct talker *k = &tk[i];
		double span_s = (double)(k->last_ns - k->first_ns) / 1e9;
		double pps = span_s > 0 ? (double)(k->frames - 1) / span_s : 0.0;

		printf("src=");
		print_mac(k->mac);
		if (pace_is_legal(pps))
			printf(" frames=%" PRIu64 " span_s=%.3f pps=%.3f rate_hz=%d\n",
			       k->frames, span_s, pps, reac_rate_snap(pps));
		else
			printf(" frames=%" PRIu64 " span_s=%.3f pps=%.3f rate_hz=n/a"
			       " reason=pps_is_not_a_legal_pace\n",
			       k->frames, span_s, pps);

		if (k->frames < min_frames) {
			printf("  VERDICT=none reason=too_few_frames have=%" PRIu64
			       " need=%" PRIu64 "\n", k->frames, min_frames);
			rc = 1;
			continue;
		}

		printf("  iv_mean_us=%.3f iv_min_us=%.3f iv_max_us=%.3f iv_sd_us=%.3f\n",
		       acc_mean_us(&k->acc), (double)k->acc.min_ns / 1000.0,
		       (double)k->acc.max_ns / 1000.0, acc_sd_us(&k->acc));
		printf("  p50_us=%.0f p90_us=%.0f p99_us=%.0f p999_us=%.0f\n",
		       acc_pct(&k->acc, 0.50), acc_pct(&k->acc, 0.90),
		       acc_pct(&k->acc, 0.99), acc_pct(&k->acc, 0.999));
		if (nominal_ns) {
			double s = span_s > 0 ? span_s : 1.0;
			printf("  nominal_us=%.3f late1p5=%" PRIu64 " late2x=%" PRIu64
			       " late4x=%" PRIu64 " catchup=%" PRIu64 "\n",
			       (double)nominal_ns / 1000.0, k->acc.late_1p5,
			       k->acc.late_2x, k->acc.late_4x, k->acc.catchup);
			printf("  late1p5_ps=%.4f late4x_ps=%.4f catchup_ps=%.4f\n",
			       (double)k->acc.late_1p5 / s, (double)k->acc.late_4x / s,
			       (double)k->acc.catchup / s);
		}
		if (want_hist) {
			for (uint32_t b = 0; b <= IV_BUCKETS; b++)
				if (k->acc.bucket[b])
					printf("  hist_us=%u count=%" PRIu64 "\n",
					       b, k->acc.bucket[b]);
		}
	}
	return rc;
}

/* ---- the control --------------------------------------------------------- *
 * Closed-form series through the SAME accumulator the pcap path uses. Each case
 * states what must be true; a failure names the case. */

static int fail(const char *what, double got, double want)
{
	fprintf(stderr, "pace_hist --self-test FAILED: %s got %.3f want %.3f\n",
	        what, got, want);
	return 1;
}

static int self_test(void)
{
	const uint64_t nom = 125000;   /* 8000 fps */
	struct pace_acc a;
	int bad = 0;

	/* 1. A perfect grid: zero late, zero catch-up, sd 0, every percentile at
	 *    the nominal. This is the NEGATIVE control -- if it reported a late
	 *    slot, every "no late slots" result would be noise. */
	acc_init(&a, nom);
	for (int i = 0; i < 100000; i++)
		acc_add(&a, nom);
	if (a.late_1p5 || a.late_2x || a.late_4x || a.catchup)
		bad |= fail("perfect grid: late/catchup", (double)(a.late_1p5 + a.catchup), 0);
	if (acc_sd_us(&a) > 0.001)
		bad |= fail("perfect grid: sd", acc_sd_us(&a), 0);
	if (acc_pct(&a, 0.999) != 125.0)
		bad |= fail("perfect grid: p99.9", acc_pct(&a, 0.999), 125.0);

	/* 2. The POSITIVE control: the same grid with three 1 ms stalls injected,
	 *    each repaid by one short interval. The instrument must find exactly
	 *    three 4x-late intervals and three catch-ups -- if it cannot see a
	 *    stall it was shown, it cannot testify that there was none. */
	acc_init(&a, nom);
	for (int i = 0; i < 100000; i++) {
		if (i == 10 || i == 500 || i == 90000) {
			acc_add(&a, 1000000);          /* 1 ms: 8x nominal   */
			acc_add(&a, 1000);             /* repaid on the grid */
		} else
			acc_add(&a, nom);
	}
	if (a.late_4x != 3)
		bad |= fail("injected stalls: late4x", (double)a.late_4x, 3);
	if (a.late_2x != 3)
		bad |= fail("injected stalls: late2x", (double)a.late_2x, 3);
	if (a.catchup != 3)
		bad |= fail("injected stalls: catchup", (double)a.catchup, 3);
	if (a.max_ns != 1000000)
		bad |= fail("injected stalls: max_ns", (double)a.max_ns, 1000000);
	/* Three stalls in 100003 intervals must NOT move the median. */
	if (acc_pct(&a, 0.50) != 125.0)
		bad |= fail("injected stalls: p50 unmoved", acc_pct(&a, 0.50), 125.0);

	/* 3. The 1.5x edge is inclusive and the 2x edge is not reached at 1.9x --
	 *    the boundary an off-by-one would hide, because a pacer that misses by
	 *    just under a period is exactly the interesting case. */
	acc_init(&a, nom);
	acc_add(&a, nom * 3 / 2);      /* exactly 1.5x: late      */
	acc_add(&a, nom * 19 / 10);    /* 1.9x: late, not 2x      */
	acc_add(&a, nom * 2);          /* exactly 2x: late and 2x */
	if (a.late_1p5 != 3)
		bad |= fail("edges: late1p5", (double)a.late_1p5, 3);
	if (a.late_2x != 1)
		bad |= fail("edges: late2x", (double)a.late_2x, 1);

	/* 4. The mean of a two-valued series is closed-form, and it is what a
	 *    stddev is computed against. */
	acc_init(&a, nom);
	for (int i = 0; i < 1000; i++)
		acc_add(&a, i & 1 ? 130000 : 120000);
	if (fabs(acc_mean_us(&a) - 125.0) > 1e-9)
		bad |= fail("two-valued: mean", acc_mean_us(&a), 125.0);
	if (fabs(acc_sd_us(&a) - 5.0) > 1e-6)
		bad |= fail("two-valued: sd", acc_sd_us(&a), 5.0);

	/* 5. The overflow bucket is reported as a floor, never clamped away. */
	acc_init(&a, nom);
	for (int i = 0; i < 10; i++)
		acc_add(&a, 20000000);         /* 20 ms, past the 8192 us range */
	if (acc_pct(&a, 0.50) != (double)IV_BUCKETS)
		bad |= fail("overflow: p50", acc_pct(&a, 0.50), (double)IV_BUCKETS);
	if (a.max_ns != 20000000)
		bad |= fail("overflow: max exact", (double)a.max_ns, 20000000);

	if (bad)
		return 1;
	printf("pace_hist --self-test: 5 cases pass "
	       "(perfect grid silent, 3 injected stalls all found, edges, mean/sd, overflow)\n");
	return 0;
}
