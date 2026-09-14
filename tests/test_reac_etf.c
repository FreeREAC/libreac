// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The ETF pacing backend, at the only three rates the console is ever driven at.
 *
 * WHAT THIS IS FOR. A launch time is ABSOLUTE. The userspace pacer's relative sleep
 * forgives a truncated period — it wakes, sends, and the next deadline is computed
 * from the grid it already holds — but a launch time that is 0.16 ns short is 0.16 ns
 * short of where the qdisc will release it, every slot, forever. At 44.1 kHz that is
 * 21 µs of phase after a minute and a whole slot after four hours. So the first test
 * here is not "does the period look right": it is that launch(n) equals the exact
 * rational grid for every one of 10 s of slots, at 3675, 4000 and 8000 fps.
 *
 * It was RED first, and the red was the naive implementation: advancing by
 * reac_pacer_period_ns(3675) = 272109 ns fails at slot 1 by 1 ns, ends the 10 s
 * window 5 750 ns adrift, and grows from there — a whole slot period of phase after
 * about eight minutes. The control below re-measures that number every run, so the
 * exactness test is never the only thing asserting it.
 *
 * The refusals are tested through the seam rather than by finding a kernel that
 * lacks SO_TXTIME: a fake setsockopt that refuses, and a TAI offset of 0 handed in.
 * Both are the conditions an operator will actually meet — a container without
 * CAP_NET_ADMIN, and a laptop whose clock nothing has disciplined. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reac_etf.h"

#include <net/if.h>       /* if_nametoindex — the qdisc probe's positive control */

#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* reac_pacer.h's REAC_CATCHUP_MAX_DEFAULT_US, restated rather than included: that
 * header pulls reac_rate_cfg.h out of a reac-pw checkout, which a standalone build
 * of this test does not have. The two must agree; if the soak's worst debt ever
 * moves, both move. */
#define REAC_CATCHUP_MAX_DEFAULT_US_MIRROR 2000u

static int fails;
#define CHECK(cond, ...) do { \
	if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	               printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* The exact grid, computed independently of the implementation: floor(n·10^9/fps).
 * n·10^9 for n = 80000 is 8·10^13, well inside uint64. */
static uint64_t ideal_ns(uint64_t n, unsigned fps)
{
	return (n * 1000000000ull) / fps;
}

/* ---- 1. the launch grid is exact over 10 s, at every pace ------------------ */
static void test_grid_is_exact(int fps)
{
	const uint64_t base = 1757800000000000000ull;   /* an arbitrary TAI instant */
	const uint64_t slots = (uint64_t)fps * 10;      /* 10 s of slots */

	struct reac_etf_grid g;
	CHECK(reac_etf_grid_init(&g, fps, base) == REAC_ETF_OK, "grid_init %d fps refused", fps);
	CHECK(g.launch_ns == base, "slot 0 launch %" PRIu64 " != base %" PRIu64, g.launch_ns, base);

	uint64_t prev = g.launch_ns;
	uint64_t worst_step_err = 0;
	for (uint64_t n = 1; n <= slots; n++) {
		uint64_t t = reac_etf_grid_advance(&g, 0);
		uint64_t want = base + ideal_ns(n, (unsigned)fps);

		/* ZERO DRIFT, to the nanosecond, at every slot — not just at the end.
		 * An implementation that drifts and then snaps back would pass an
		 * endpoint-only check while putting every frame in between on the wrong
		 * instant. */
		CHECK(t == want, "%d fps slot %" PRIu64 ": launch %" PRIu64 " want %" PRIu64
		      " (adrift %" PRId64 " ns)", fps, n, t, want, (int64_t)(t - want));
		if (t != want)
			return;   /* one line per rate is enough; the rest would repeat */

		/* Each step is the quotient or the quotient + 1, never anything else:
		 * the grid paces, it does not batch up a correction and pay it in a jump. */
		uint64_t step = t - prev;
		uint64_t q = 1000000000ull / (unsigned)fps;
		CHECK(step == q || step == q + 1, "%d fps slot %" PRIu64 ": step %" PRIu64
		      " is neither %" PRIu64 " nor %" PRIu64, fps, n, step, q, q + 1);
		if (step > q)
			worst_step_err = step - q;
		prev = t;
	}

	/* And the long-run rate is EXACTLY fps: 10 s of slots span exactly 10^10 ns
	 * where the rate divides it, and floor(10·10^9) where it does not. */
	uint64_t span = g.launch_ns - base;
	CHECK(span == ideal_ns(slots, (unsigned)fps),
	      "%d fps: 10 s of slots spans %" PRIu64 " ns, want %" PRIu64,
	      fps, span, ideal_ns(slots, (unsigned)fps));
	printf("  %4d fps: %" PRIu64 " slots, span %" PRIu64 " ns, drift 0 ns, "
	       "step jitter %" PRIu64 " ns (the remainder, by construction)\n",
	       fps, slots, span, worst_step_err);
}

/* The control for the test above: the NAIVE grid — advance by the rounded period the
 * userspace pacer uses — must FAIL the same check at 44.1 kHz. A drift test that
 * cannot tell a drifting grid from an exact one is not measuring drift. */
static void test_naive_grid_would_drift(void)
{
	const int fps = 3675;
	const long rounded = (long)(1000000000.0 / fps + 0.5);   /* reac_pacer_period_ns */
	const uint64_t slots = (uint64_t)fps * 10;

	uint64_t naive = 0;
	for (uint64_t n = 0; n < slots; n++)
		naive += (uint64_t)rounded;

	uint64_t exact = ideal_ns(slots, fps);
	int64_t drift = (int64_t)naive - (int64_t)exact;
	CHECK(drift != 0, "the naive grid did not drift — this control proves nothing");
	/* The bar is the exactness test's own resolution: the control has to drift by
	 * more than the 1 ns that test can see, or a green there would prove nothing.
	 * It drifts by 5 750 ns, three and a half orders of magnitude clear of it. */
	CHECK(drift > 1000, "the naive grid drifted only %" PRId64 " ns over 10 s; "
	      "the exactness test above could barely have caught it", drift);
	printf("  control: the rounded-period grid is %+" PRId64 " ns adrift after 10 s "
	       "at 3675 fps (%.2f us, %.1f us/min), and a whole slot period out after "
	       "%.0f s\n", drift, drift / 1000.0, drift * 6.0 / 1000.0,
	       272108.0 / (drift / 10.0));
}

/* ---- 2. a steered slot advances by the steer, and does not lose the phase --- */
static void test_steer_does_not_reset_the_remainder(void)
{
	const uint64_t base = 1757800000000000000ull;
	struct reac_etf_grid a, b;
	reac_etf_grid_init(&a, 3675, base);
	reac_etf_grid_init(&b, 3675, base);

	/* Run `a` nominal for 1000 slots. Run `b` the same, but with slot 500 steered
	 * to 272 200 ns by the clock discipline. */
	for (int n = 1; n <= 1000; n++) {
		reac_etf_grid_advance(&a, 0);
		reac_etf_grid_advance(&b, n == 500 ? 272200 : 0);
	}
	int64_t delta = (int64_t)b.launch_ns - (int64_t)a.launch_ns;

	/* The steered slot was 272 200 instead of the 272 108 or 272 109 the nominal
	 * grid would have taken, so the two grids part by that difference and by
	 * NOTHING ELSE. */
	CHECK(delta == 91 || delta == 92, "a single steered slot moved the grid by %"
	      PRId64 " ns; only the steer itself may move it", delta);

	/* THE REMAINDER WAS CARRIED, NOT RESET. A steered slot consumes no remainder,
	 * so `b`'s accumulator is legitimately one nominal step behind `a`'s — what it
	 * must never be is zero, which is what an implementation that re-initialised
	 * the grid on every steer would leave. */
	CHECK(b.frac != 0, "the steered slot reset the remainder accumulator to 0");

	/* And the divergence is BOUNDED: 5000 more nominal slots must not widen it by
	 * one nanosecond. A steer that quietly re-truncated the period would show up
	 * here as a growing gap, which is the whole failure this test exists for. */
	for (int n = 0; n < 5000; n++) {
		reac_etf_grid_advance(&a, 0);
		reac_etf_grid_advance(&b, 0);
	}
	int64_t delta2 = (int64_t)b.launch_ns - (int64_t)a.launch_ns;
	CHECK(delta2 == delta, "5000 slots after one steer the grids are %" PRId64
	      " ns apart, were %" PRId64 " — the steer re-truncated the period",
	      delta2, delta);
	printf("  steer: one steered slot moves the grid by %+" PRId64 " ns, carries the "
	       "remainder (frac %u), and 5000 slots later the gap is still %+" PRId64 " ns\n",
	       delta, b.frac, delta2);
}

/* ---- 3. the backend refuses without SO_TXTIME support ---------------------- */
static int fake_setopt_refuses(int fd, int level, int optname,
                               const void *val, socklen_t len)
{
	(void)fd; (void)level; (void)val; (void)len;
	/* An old kernel answers ENOPROTOOPT for SO_TXTIME; anything else must not be
	 * mistaken for support. */
	if (optname == SO_TXTIME) {
		errno = ENOPROTOOPT;
		return -1;
	}
	return 0;
}

static int fake_setopt_accepts_calls;
static int fake_setopt_clockid;
static unsigned fake_setopt_flags;
static int fake_setopt_accepts(int fd, int level, int optname,
                               const void *val, socklen_t len)
{
	(void)fd; (void)level;
	if (optname == SO_TXTIME && len >= 8 && val) {
		const int32_t *p = val;
		fake_setopt_clockid = p[0];
		fake_setopt_flags = (unsigned)p[1];
		fake_setopt_accepts_calls++;
	}
	return 0;
}

static void test_refusals(void)
{
	/* No SO_TXTIME: refused by code, with a TAI offset that is otherwise fine. */
	enum reac_etf_refusal r = reac_etf_socket_arm(-1, 37, fake_setopt_refuses);
	CHECK(r == REAC_ETF_REFUSE_NO_TXTIME,
	      "a kernel without SO_TXTIME gave %d (%s), want REAC_ETF_REFUSE_NO_TXTIME",
	      (int)r, reac_etf_refusal_name(r));

	/* TAI offset 0: refused BEFORE the socket is touched, because a clock nobody
	 * has disciplined would put every launch time 37 s from where the qdisc reads
	 * it, and the frames would be dropped as expired. */
	fake_setopt_accepts_calls = 0;
	r = reac_etf_socket_arm(-1, 0, fake_setopt_accepts);
	CHECK(r == REAC_ETF_REFUSE_TAI_UNSET,
	      "a zero TAI offset gave %d (%s), want REAC_ETF_REFUSE_TAI_UNSET",
	      (int)r, reac_etf_refusal_name(r));
	CHECK(fake_setopt_accepts_calls == 0,
	      "the socket was armed before the TAI offset was checked");

	/* The happy path arms CLOCK_TAI in STRICT mode: no SOF_TXTIME_DEADLINE_MODE,
	 * because deadline mode lets the qdisc send EARLY and a slave recovers its word
	 * clock from the interval — early is as wrong as late. */
	fake_setopt_accepts_calls = 0;
	r = reac_etf_socket_arm(-1, 37, fake_setopt_accepts);
	CHECK(r == REAC_ETF_OK, "the armed path gave %d (%s)", (int)r,
	      reac_etf_refusal_name(r));
	CHECK(fake_setopt_accepts_calls == 1, "SO_TXTIME was set %d times, want 1",
	      fake_setopt_accepts_calls);
	CHECK(fake_setopt_clockid == CLOCK_TAI,
	      "SO_TXTIME armed clockid %d, want CLOCK_TAI (%d)", fake_setopt_clockid,
	      (int)CLOCK_TAI);
	CHECK((fake_setopt_flags & 1u) == 0,
	      "SOF_TXTIME_DEADLINE_MODE is set; the qdisc may then release EARLY");
	CHECK((fake_setopt_flags & 2u) != 0,
	      "SOF_TXTIME_REPORT_ERRORS is clear; a dropped frame would vanish silently");

	/* Every refusal has a name. A code an operator cannot read is a code they guess. */
	for (int i = REAC_ETF_OK; i <= REAC_ETF_REFUSE_BAD_LEAD; i++)
		CHECK(reac_etf_refusal_name((enum reac_etf_refusal)i)[0] != '\0',
		      "refusal %d has an empty name", i);

	printf("  refusals: no SO_TXTIME and TAI offset 0 both refuse by code; the armed "
	       "socket is CLOCK_TAI, strict mode, errors reported\n");
}

/* ---- 4. the lead is bounded ----------------------------------------------- */
static void test_lead_bounds(void)
{
	CHECK(reac_etf_lead_check(REAC_ETF_LEAD_US_DEFAULT) == REAC_ETF_OK,
	      "the default lead is outside its own bounds");
	CHECK(reac_etf_lead_check(REAC_ETF_LEAD_US_MIN - 1) == REAC_ETF_REFUSE_BAD_LEAD,
	      "a lead under the qdisc's own delta was accepted");
	CHECK(reac_etf_lead_check(REAC_ETF_LEAD_US_MAX + 1) == REAC_ETF_REFUSE_BAD_LEAD,
	      "a 50 ms+ lead was accepted");
	CHECK(reac_etf_lead_check(0) == REAC_ETF_REFUSE_BAD_LEAD, "a zero lead was accepted");
	/* The default is DERIVED, not chosen: the pacer's measured worst single slot
	 * debt (2000 us, the 30-minute soak) plus the qdisc delta proven on the
	 * repacer's rig ports (300 us), rounded up. A change to it is a change to one
	 * of those two measurements and has to say which. */
	CHECK(REAC_ETF_LEAD_US_DEFAULT == 2500u,
	      "the default lead is no longer 2000 us (measured worst debt) + 300 us "
	      "(proven qdisc delta), rounded up");
	CHECK(REAC_ETF_LEAD_US_DEFAULT >= REAC_CATCHUP_MAX_DEFAULT_US_MIRROR,
	      "the lead is shorter than the worst wake tail the pacer has measured");
	printf("  lead: default %u us = 2000 (measured worst slot debt) + 300 (proven "
	       "qdisc delta), bounds [%u, %u]\n",
	       REAC_ETF_LEAD_US_DEFAULT, REAC_ETF_LEAD_US_MIN, REAC_ETF_LEAD_US_MAX);
}

/* ---- 5. the qdisc probe can SEE a qdisc before it reports one absent --------
 *
 * The probe's own control. An instrument that has not been shown to detect the
 * presence cannot testify about the absence, and "no etf qdisc on this NIC" is an
 * absence claim that REFUSES THE BACKEND. `lo` exists on every machine and every
 * container, and its root qdisc is a real qdisc with a real name — so the probe
 * must come back readable and NAME it. A probe that returned UNREADABLE here would
 * be refusing correctly configured rigs. */
static void test_qdisc_probe_reads_a_real_device(void)
{
	unsigned idx = if_nametoindex("lo");
	if (!idx) {
		printf("  qdisc: NO CONTROL RUN — this host has no `lo` to probe\n");
		fails++;
		return;
	}
	char kind[32];
	enum reac_etf_qdisc q = reac_etf_qdisc_probe((int)idx, kind, sizeof kind);
	CHECK(q != REAC_ETF_QDISC_UNREADABLE,
	      "the qdisc dump was unreadable on `lo` — every refusal this probe makes "
	      "would be a false one");
	CHECK(kind[0] != '\0',
	      "the probe read the dump but named no root qdisc for `lo`; it cannot "
	      "detect a presence, so its absences mean nothing");
	/* `lo` is not an ETF device, so the verdict must be ABSENT — the probe
	 * distinguishing "read it, no etf" from "could not read" is the whole point. */
	CHECK(q == REAC_ETF_QDISC_ABSENT,
	      "`lo` probed as %d; a loopback carries no etf qdisc", (int)q);
	printf("  qdisc: probe reads `lo` root qdisc \"%s\" and reports ABSENT — it can "
	       "see a presence, so its absence is evidence\n", kind);
}

/* ---- 6. the control message carries the launch time, byte for byte --------- */
static void test_stamp_carries_the_launch_time(void)
{
	uint8_t buf[REAC_ETF_CMSG_SPACE];
	struct iovec iov = { .iov_base = (void *)"x", .iov_len = 1 };
	struct msghdr msg;
	memset(&msg, 0, sizeof msg);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	const uint64_t launch = 1757800000123456789ull;
	size_t n = reac_etf_stamp(&msg, buf, launch);
	CHECK(n == REAC_ETF_CMSG_SPACE, "stamp used %zu control bytes, want %zu",
	      n, (size_t)REAC_ETF_CMSG_SPACE);

	struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
	CHECK(c != NULL, "no control message was attached");
	if (!c)
		return;
	CHECK(c->cmsg_level == SOL_SOCKET, "cmsg_level %d, want SOL_SOCKET", c->cmsg_level);
	CHECK(c->cmsg_type == SCM_TXTIME, "cmsg_type %d, want SCM_TXTIME (%d)",
	      c->cmsg_type, (int)SCM_TXTIME);
	uint64_t got;
	memcpy(&got, CMSG_DATA(c), sizeof got);
	CHECK(got == launch, "the stamped launch time is %" PRIu64 ", want %" PRIu64,
	      got, launch);
	printf("  stamp: SCM_TXTIME carries the launch time unchanged\n");
}

int main(void)
{
	printf("test_reac_etf: the ETF pacing backend\n");
	test_naive_grid_would_drift();
	for (int i = 0; i < 3; i++) {
		static const int rates[3] = { 3675, 4000, 8000 };
		test_grid_is_exact(rates[i]);
	}
	test_steer_does_not_reset_the_remainder();
	test_refusals();
	test_lead_bounds();
	test_qdisc_probe_reads_a_real_device();
	test_stamp_carries_the_launch_time();

	/* The real TAI offset on THIS machine, printed rather than asserted: a build
	 * host is not the desk, and a container's clock is the host's. It is the number
	 * the operator needs when a refusal fires. */
	printf("  this host: TAI offset %d s\n", reac_etf_tai_offset());

	if (fails) {
		printf("test_reac_etf: %d FAILED\n", fails);
		return 1;
	}
	printf("test_reac_etf: all checks passed\n");
	return 0;
}
