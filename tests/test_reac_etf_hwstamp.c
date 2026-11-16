// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The hardware launch's instrument of record (transport/src/reac_etf_hwstamp.c): the
 * NIC's TX stamp of a sampled frame, paired with the launch time that frame carried.
 *
 * WHAT CAN GO WRONG, AND WHICH ARM CATCHES IT.
 *  - Pairing by ORDER instead of by the grid: one stamp the NIC skipped (it stamps a few
 *    outstanding frames at a time) shifts every later pair by a whole period, and the
 *    "launch error" becomes the period. PAIRS_BY_THE_GRID drops a stamp and still wants
 *    every error exact.
 *  - A stamp that pairs with nothing being dropped silently: a PHC not aligned to TAI
 *    then reads as "fewer samples" rather than as the fault it is. UNPAIRED counts it.
 *  - The count the figures are made of being lost: COUNTS wants paired == what paired.
 *  - Percentiles and spread read off the wrong bin: KNOWN_DISTRIBUTION puts a known
 *    set of errors in and wants its mean, sd, min, max and p50/p99/p99.9 back.
 *  - The error-queue reader taking a SOFTWARE stamp (ts[0]) for the hardware one, or a
 *    message that is not a timestamp (an ETF drop report) for a stamp: STAMP_OF builds
 *    each message the kernel can hand back and wants only the raw hardware one read. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reac_etf_hwstamp.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <linux/errqueue.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __func__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PERIOD 250000u   /* 4000 fps */
#define T0     1763240000000000000ull

static struct reac_etf_hw S;

static void pairs_by_the_grid(void)
{
	reac_etf_hw_init(&S, PERIOD);
	/* 8 sampled launches; the NIC stamps all but the third, each 37 ns late. */
	for (int i = 0; i < 8; i++)
		reac_etf_hw_expect(&S, T0 + (uint64_t)i * PERIOD);
	int paired = 0;
	for (int i = 0; i < 8; i++)
		if (i != 2)
			paired += reac_etf_hw_take(&S, T0 + (uint64_t)i * PERIOD + 37);
	struct reac_etf_hw_summary r;
	reac_etf_hw_read(&S, &r);
	CHECK(paired == 7, "7 stamps paired, got %d", paired);
	CHECK(r.paired == 7 && r.unpaired == 0, "paired 7 unpaired 0, got %" PRIu64 " %" PRIu64,
	      r.paired, r.unpaired);
	CHECK(r.min_ns == 37 && r.max_ns == 37, "every error 37 ns, got min %" PRId64 " max %" PRId64,
	      r.min_ns, r.max_ns);
	CHECK(fabs(r.mean_ns - 37.0) < 1e-9, "mean 37, got %.3f", r.mean_ns);
	/* early stamps pair too (a negative launch error is a real result) */
	reac_etf_hw_init(&S, PERIOD);
	reac_etf_hw_expect(&S, T0);
	CHECK(reac_etf_hw_take(&S, T0 - 120) == 1, "an early stamp pairs");
	reac_etf_hw_read(&S, &r);
	CHECK(r.min_ns == -120, "early by 120 ns reads -120, got %" PRId64, r.min_ns);
}

static void unpaired(void)
{
	reac_etf_hw_init(&S, PERIOD);
	reac_etf_hw_expect(&S, T0);
	/* a PHC 37 s off TAI (the UTC offset nobody applied): nothing within half a period */
	CHECK(reac_etf_hw_take(&S, T0 + 37000000000ull) == 0, "a stamp 37 s off does not pair");
	/* just beyond half a period: still not ours */
	CHECK(reac_etf_hw_take(&S, T0 + PERIOD / 2 + 1) == 0, "beyond half a period does not pair");
	/* a launch is paired once: its second stamp is unpaired */
	CHECK(reac_etf_hw_take(&S, T0 + 5) == 1, "the real stamp pairs");
	CHECK(reac_etf_hw_take(&S, T0 + 5) == 0, "a launch pairs once");
	struct reac_etf_hw_summary r;
	reac_etf_hw_read(&S, &r);
	CHECK(r.paired == 1 && r.unpaired == 3, "paired 1 unpaired 3, got %" PRIu64 " %" PRIu64,
	      r.paired, r.unpaired);
	/* an empty instrument says zero samples and nothing else */
	reac_etf_hw_init(&S, PERIOD);
	reac_etf_hw_read(&S, &r);
	CHECK(r.paired == 0 && r.unpaired == 0 && r.p99_ns == 0, "empty reads zero");
}

static void known_distribution(void)
{
	/* 1000 stamps: 990 at +100 ns, 9 at +500 ns, 1 at +20 us (beyond the histogram). */
	reac_etf_hw_init(&S, PERIOD);
	uint64_t t = T0;
	for (int i = 0; i < 1000; i++, t += PERIOD) {
		int64_t e = i < 990 ? 100 : i < 999 ? 500 : 20000;
		reac_etf_hw_expect(&S, t);
		reac_etf_hw_take(&S, t + (uint64_t)e);
	}
	struct reac_etf_hw_summary r;
	reac_etf_hw_read(&S, &r);
	double mean = (990.0 * 100 + 9.0 * 500 + 20000) / 1000.0;
	CHECK(r.paired == 1000, "1000 paired, got %" PRIu64, r.paired);
	CHECK(fabs(r.mean_ns - mean) < 1e-6, "mean %.3f, got %.3f", mean, r.mean_ns);
	CHECK(r.min_ns == 100 && r.max_ns == 20000, "min 100 max 20000, got %" PRId64 " %" PRId64,
	      r.min_ns, r.max_ns);
	CHECK(r.outside == 1, "one stamp beyond the histogram, got %" PRIu64, r.outside);
	CHECK(r.p50_ns == 100, "p50 in the 100 ns bin, got %" PRId64, r.p50_ns);
	CHECK(r.p99_ns == 100, "p99 is the 990th: 100 ns bin, got %" PRId64, r.p99_ns);
	CHECK(r.p999_ns == 500, "p99.9 is the 999th: 500 ns bin, got %" PRId64, r.p999_ns);
	/* the spread of the 999 inside the histogram, at bin centres (+5 ns) */
	double m = r.mean_ns, v = (990.0 * pow(105 - m, 2) + 9.0 * pow(505 - m, 2)) / 999.0;
	CHECK(fabs(r.sd_ns - sqrt(v)) < 1e-6, "sd %.3f, got %.3f", sqrt(v), r.sd_ns);
}

/* ---- the error-queue reader ---------------------------------------------- */

struct fake_msg {
	struct msghdr m;
	union { char buf[512]; struct cmsghdr align; } u;
	struct cmsghdr *c;
};

static void fake_begin(struct fake_msg *f)
{
	memset(f, 0, sizeof *f);
	f->m.msg_control = f->u.buf;
	f->m.msg_controllen = sizeof f->u.buf;
	f->c = CMSG_FIRSTHDR(&f->m);
}

static void fake_stamps(struct fake_msg *f, uint64_t sw_ns, uint64_t hw_ns)
{
	struct scm_timestamping ts;
	memset(&ts, 0, sizeof ts);
	ts.ts[0].tv_sec = (time_t)(sw_ns / 1000000000ull);
	ts.ts[0].tv_nsec = (long)(sw_ns % 1000000000ull);
	ts.ts[2].tv_sec = (time_t)(hw_ns / 1000000000ull);
	ts.ts[2].tv_nsec = (long)(hw_ns % 1000000000ull);
	f->c->cmsg_level = SOL_SOCKET;
	f->c->cmsg_type = SO_TIMESTAMPING;
	f->c->cmsg_len = CMSG_LEN(sizeof ts);
	memcpy(CMSG_DATA(f->c), &ts, sizeof ts);
	f->c = (struct cmsghdr *)((char *)f->c + CMSG_SPACE(sizeof ts));
}

static void fake_ee(struct fake_msg *f, int level, int type, uint8_t origin)
{
	struct sock_extended_err ee;
	memset(&ee, 0, sizeof ee);
	ee.ee_origin = origin;
	ee.ee_errno = origin == SO_EE_ORIGIN_TIMESTAMPING ? ENOMSG : ECANCELED;
	f->c->cmsg_level = level;
	f->c->cmsg_type = type;
	f->c->cmsg_len = CMSG_LEN(sizeof ee);
	memcpy(CMSG_DATA(f->c), &ee, sizeof ee);
	f->c = (struct cmsghdr *)((char *)f->c + CMSG_SPACE(sizeof ee));
}

static void fake_end(struct fake_msg *f)
{
	f->m.msg_controllen = (size_t)((char *)f->c - f->u.buf);
}

static void stamp_of(void)
{
	struct fake_msg f;
	uint64_t hw = 0;
	const uint64_t SW = T0 - 900000, HW = T0 + 41;

	/* what the kernel hands back on a packet socket: SCM_TIMESTAMPING, then the
	 * extended error at SOL_PACKET / PACKET_TX_TIMESTAMP */
	fake_begin(&f);
	fake_stamps(&f, SW, HW);
	fake_ee(&f, SOL_PACKET, PACKET_TX_TIMESTAMP, SO_EE_ORIGIN_TIMESTAMPING);
	fake_end(&f);
	CHECK(reac_etf_hw_stamp_of(&f.m, &hw) == 1 && hw == HW,
	      "the raw hardware stamp is read, got %" PRIu64 " want %" PRIu64, hw, (uint64_t)HW);

	/* a software stamp alone (ts[2] zero) is not the instrument */
	fake_begin(&f);
	fake_stamps(&f, SW, 0);
	fake_ee(&f, SOL_PACKET, PACKET_TX_TIMESTAMP, SO_EE_ORIGIN_TIMESTAMPING);
	fake_end(&f);
	CHECK(reac_etf_hw_stamp_of(&f.m, &hw) == 0, "a software-only stamp is refused");

	/* an ETF drop report (origin TXTIME) is not a stamp, whatever else rides with it */
	fake_begin(&f);
	fake_ee(&f, SOL_PACKET, PACKET_TX_TIMESTAMP, SO_EE_ORIGIN_TXTIME);
	fake_end(&f);
	CHECK(reac_etf_hw_stamp_of(&f.m, &hw) == 0, "an ETF drop report is not a stamp");

	/* an IP socket's error queue says it at SOL_IP / IP_RECVERR: the same reading */
	fake_begin(&f);
	fake_stamps(&f, SW, HW);
	fake_ee(&f, SOL_IP, IP_RECVERR, SO_EE_ORIGIN_TIMESTAMPING);
	fake_end(&f);
	hw = 0;
	CHECK(reac_etf_hw_stamp_of(&f.m, &hw) == 1 && hw == HW, "the IP-level report reads too");

	/* the request a frame carries */
	union { char b[REAC_ETF_HW_CMSG_SPACE]; struct cmsghdr a; } req;
	memset(&req, 0, sizeof req);
	reac_etf_hw_request(&req.a);
	uint32_t v;
	memcpy(&v, CMSG_DATA(&req.a), sizeof v);
	CHECK(req.a.cmsg_level == SOL_SOCKET && req.a.cmsg_type == SO_TIMESTAMPING &&
	      req.a.cmsg_len == CMSG_LEN(sizeof v) && v == SOF_TIMESTAMPING_TX_HARDWARE,
	      "the request is SOL_SOCKET/SO_TIMESTAMPING TX_HARDWARE");
}

int main(void)
{
	pairs_by_the_grid();
	unpaired();
	known_distribution();
	stamp_of();
	if (fails) {
		printf("test_reac_etf_hwstamp: %d FAILED\n", fails);
		return 1;
	}
	printf("test_reac_etf_hwstamp: all pass\n");
	return 0;
}
