// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_etf_hwstamp — see reac_etf_hwstamp.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reac_etf_hwstamp.h"

#include <errno.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/errqueue.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>

#define RLX memory_order_relaxed

void reac_etf_hw_init(struct reac_etf_hw *s, uint64_t period_ns)
{
	memset(s->pending, 0, sizeof s->pending);
	s->next = 0;
	s->half_period_ns = period_ns / 2;
	for (unsigned i = 0; i < REAC_ETF_HW_BINS; i++)
		atomic_store_explicit(&s->bins[i], 0, RLX);
	atomic_store_explicit(&s->under, 0, RLX);
	atomic_store_explicit(&s->over, 0, RLX);
	atomic_store_explicit(&s->paired, 0, RLX);
	atomic_store_explicit(&s->unpaired, 0, RLX);
	atomic_store_explicit(&s->sum_ns, 0, RLX);
	atomic_store_explicit(&s->min_ns, INT64_MAX, RLX);
	atomic_store_explicit(&s->max_ns, INT64_MIN, RLX);
}

void reac_etf_hw_expect(struct reac_etf_hw *s, uint64_t launch_ns)
{
	/* A ring: the oldest launch still waiting is overwritten, which is a stamp the NIC
	 * never sent; it simply never pairs. */
	s->pending[s->next] = launch_ns;
	s->next = (s->next + 1) % REAC_ETF_HW_PENDING;
}

int reac_etf_hw_take(struct reac_etf_hw *s, uint64_t hw_ns)
{
	int best = -1;
	uint64_t best_d = 0;
	for (int i = 0; i < REAC_ETF_HW_PENDING; i++) {
		if (!s->pending[i])
			continue;
		uint64_t d = hw_ns > s->pending[i] ? hw_ns - s->pending[i] : s->pending[i] - hw_ns;
		if (d <= s->half_period_ns && (best < 0 || d < best_d)) {
			best = i;
			best_d = d;
		}
	}
	if (best < 0) {
		atomic_store_explicit(&s->unpaired, atomic_load_explicit(&s->unpaired, RLX) + 1, RLX);
		return 0;
	}
	int64_t err = (int64_t)(hw_ns - s->pending[best]);
	s->pending[best] = 0;

	if (err < -REAC_ETF_HW_SPAN_NS)
		atomic_store_explicit(&s->under, atomic_load_explicit(&s->under, RLX) + 1, RLX);
	else if (err >= REAC_ETF_HW_SPAN_NS)
		atomic_store_explicit(&s->over, atomic_load_explicit(&s->over, RLX) + 1, RLX);
	else {
		unsigned b = (unsigned)((err + REAC_ETF_HW_SPAN_NS) / REAC_ETF_HW_BIN_NS);
		atomic_store_explicit(&s->bins[b], atomic_load_explicit(&s->bins[b], RLX) + 1, RLX);
	}
	atomic_store_explicit(&s->sum_ns, atomic_load_explicit(&s->sum_ns, RLX) + err, RLX);
	if (err < atomic_load_explicit(&s->min_ns, RLX))
		atomic_store_explicit(&s->min_ns, err, RLX);
	if (err > atomic_load_explicit(&s->max_ns, RLX))
		atomic_store_explicit(&s->max_ns, err, RLX);
	/* paired last: a reader that sees the count sees what it counts */
	atomic_store_explicit(&s->paired, atomic_load_explicit(&s->paired, RLX) + 1,
	                      memory_order_release);
	return 1;
}

/* The lower edge of the bin holding the q-th quantile of the histogram's samples. */
static int64_t quantile(const uint32_t *bins, uint64_t under, uint64_t n, double q)
{
	uint64_t want = (uint64_t)ceil(q * (double)n - 1e-9);   /* 0.999 * 1000 is 999, not 1000 */
	if (want == 0)
		want = 1;
	uint64_t seen = under;
	if (seen >= want)
		return -REAC_ETF_HW_SPAN_NS;
	for (unsigned i = 0; i < REAC_ETF_HW_BINS; i++) {
		seen += bins[i];
		if (seen >= want)
			return (int64_t)i * REAC_ETF_HW_BIN_NS - REAC_ETF_HW_SPAN_NS;
	}
	return REAC_ETF_HW_SPAN_NS;
}

void reac_etf_hw_read(const struct reac_etf_hw *s, struct reac_etf_hw_summary *out)
{
	memset(out, 0, sizeof *out);
	uint64_t n = atomic_load_explicit(&s->paired, memory_order_acquire);
	out->paired = n;
	out->unpaired = atomic_load_explicit(&s->unpaired, RLX);
	if (!n)
		return;
	static uint32_t bins[REAC_ETF_HW_BINS];   /* the reader's copy; one reader at a time */
	uint64_t under = atomic_load_explicit(&s->under, RLX);
	uint64_t over = atomic_load_explicit(&s->over, RLX);
	double mean = (double)atomic_load_explicit(&s->sum_ns, RLX) / (double)n;
	double var = 0.0;
	uint64_t inside = 0;
	for (unsigned i = 0; i < REAC_ETF_HW_BINS; i++) {
		bins[i] = atomic_load_explicit(&s->bins[i], RLX);
		inside += bins[i];
		double mid = (double)i * REAC_ETF_HW_BIN_NS - REAC_ETF_HW_SPAN_NS + REAC_ETF_HW_BIN_NS / 2.0;
		var += (double)bins[i] * (mid - mean) * (mid - mean);
	}
	out->mean_ns = mean;
	out->sd_ns = inside ? sqrt(var / (double)inside) : 0.0;
	out->min_ns = atomic_load_explicit(&s->min_ns, RLX);
	out->max_ns = atomic_load_explicit(&s->max_ns, RLX);
	out->outside = under + over;
	uint64_t total = under + inside + over;
	out->p50_ns = quantile(bins, under, total, 0.5);
	out->p99_ns = quantile(bins, under, total, 0.99);
	out->p999_ns = quantile(bins, under, total, 0.999);
}

/* ---- the socket ----------------------------------------------------------- */

int reac_etf_hw_arm(int fd, const char *ifname)
{
	if (fd < 0 || !ifname || !*ifname)
		return -EINVAL;
	struct ifreq ifr;
	struct hwtstamp_config cfg;
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	ifr.ifr_data = (char *)&cfg;

	/* THE RX FILTER IS SOMEBODY ELSE'S. Read what the device has and change only the
	 * TX side, so a ptp4l stamping PTP on the same NIC keeps its receive filter. */
	memset(&cfg, 0, sizeof cfg);
	if (ioctl(fd, SIOCGHWTSTAMP, &ifr) < 0)
		memset(&cfg, 0, sizeof cfg);   /* a driver without the getter: rx filter none */
	cfg.flags = 0;
	cfg.tx_type = HWTSTAMP_TX_ON;
	if (ioctl(fd, SIOCSHWTSTAMP, &ifr) < 0)
		return -errno;

	/* The socket REPORTS raw hardware stamps; which frames are stamped is each
	 * frame's own request. OPT_TSONLY: the stamp comes back without the frame. */
	unsigned flags = SOF_TIMESTAMPING_RAW_HARDWARE | SOF_TIMESTAMPING_OPT_TSONLY;
	if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof flags) < 0)
		return -errno;
	return 0;
}

void reac_etf_hw_request(struct cmsghdr *c)
{
	uint32_t req = SOF_TIMESTAMPING_TX_HARDWARE;
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SO_TIMESTAMPING;
	c->cmsg_len = CMSG_LEN(sizeof req);
	memcpy(CMSG_DATA(c), &req, sizeof req);
}

int reac_etf_hw_stamp_of(struct msghdr *m, uint64_t *hw_ns)
{
	int stamped = 0, is_tstamp = 0;
	uint64_t v = 0;
	for (struct cmsghdr *c = CMSG_FIRSTHDR(m); c; c = CMSG_NXTHDR(m, c)) {
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMPING &&
		    c->cmsg_len >= CMSG_LEN(sizeof(struct scm_timestamping))) {
			struct scm_timestamping ts;
			memcpy(&ts, CMSG_DATA(c), sizeof ts);
			/* ts[2] is the raw hardware stamp; ts[0] would be software */
			if (ts.ts[2].tv_sec || ts.ts[2].tv_nsec) {
				v = (uint64_t)ts.ts[2].tv_sec * 1000000000ull + (uint64_t)ts.ts[2].tv_nsec;
				stamped = 1;
			}
		} else if (((c->cmsg_level == SOL_PACKET && c->cmsg_type == PACKET_TX_TIMESTAMP) ||
		            (c->cmsg_level == SOL_IP && c->cmsg_type == IP_RECVERR) ||
		            (c->cmsg_level == SOL_IPV6 && c->cmsg_type == IPV6_RECVERR)) &&
		           c->cmsg_len >= CMSG_LEN(sizeof(struct sock_extended_err))) {
			struct sock_extended_err ee;
			memcpy(&ee, CMSG_DATA(c), sizeof ee);
			if (ee.ee_origin == SO_EE_ORIGIN_TIMESTAMPING)
				is_tstamp = 1;
		}
	}
	if (stamped && is_tstamp) {
		*hw_ns = v;
		return 1;
	}
	return 0;
}
