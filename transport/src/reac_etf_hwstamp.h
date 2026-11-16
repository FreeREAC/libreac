// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_etf_hwstamp — the hardware launch's instrument of record: the NIC's own TX stamp
 * of a frame, against the launch time the frame carried (docs/ETF-PACING.md, "Acceptance:
 * hardware stamps are the instrument of record").
 *
 * WHY NOT A CAPTURE. tcpdump on the transmitting device stamps a frame in software as it
 * reaches the driver, which under offload is BEFORE the NIC holds it for its launch
 * time: it cannot see what the offload does. The i226 stamps a frame with its PHC as the
 * frame leaves, and that stamp minus the frame's SCM_TXTIME is the launch error.
 *
 * A SAMPLE, AND IT SAYS SO. The NIC stamps a few outstanding frames at a time, so every
 * Nth frame asks for a stamp (a per-frame SO_TIMESTAMPING cmsg beside its SCM_TXTIME),
 * and every figure read here carries the count of stamps it is made of.
 *
 * PAIRED BY THE GRID, NOT BY ORDER. A stamp is paired with the sampled launch time
 * within half a slot period of it. The error being judged is well under 1 us against a
 * period of 125 us or more, so a pairing is unambiguous, and a stamp the NIC skipped
 * shifts nothing. A stamp that pairs with no sampled launch is counted UNPAIRED: that is
 * what a PHC not aligned to CLOCK_TAI looks like, and it is reported, never dropped.
 *
 * ONE WRITER. The pacer thread pairs and counts; any other thread reads the counters
 * (relaxed atomics, single writer), so the slot path takes no lock.
 *
 * Private to transport/src: no public struct grows for it. */
#ifndef REAC_ETF_HWSTAMP_H
#define REAC_ETF_HWSTAMP_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/* The histogram: 10 ns bins from -10 us to +10 us, beyond which a stamp is counted in
 * the under or over bin with its exact value still in min and max. */
#define REAC_ETF_HW_BIN_NS     10
#define REAC_ETF_HW_SPAN_NS    10000
#define REAC_ETF_HW_BINS       (2 * REAC_ETF_HW_SPAN_NS / REAC_ETF_HW_BIN_NS)
/* How many sampled launch times wait for their stamp. */
#define REAC_ETF_HW_PENDING    64

struct reac_etf_hw {
	/* the pacer thread's alone */
	uint64_t pending[REAC_ETF_HW_PENDING];  /* sampled launch times, 0 = free */
	unsigned next;
	uint64_t half_period_ns;
	/* written by the pacer thread, read by anyone */
	_Atomic uint32_t bins[REAC_ETF_HW_BINS];
	_Atomic uint64_t under, over;           /* paired, outside the histogram */
	_Atomic uint64_t paired, unpaired;
	_Atomic int64_t  sum_ns;
	_Atomic int64_t  min_ns, max_ns;
};

/* What a reader is told. Every figure is over `paired` stamps; `unpaired` stamps found no
 * launch time within half a period. Percentiles are bin edges, REAC_ETF_HW_BIN_NS wide. */
struct reac_etf_hw_summary {
	uint64_t paired, unpaired;
	double   mean_ns, sd_ns;
	int64_t  min_ns, max_ns;
	int64_t  p50_ns, p99_ns, p999_ns;
	uint64_t outside;   /* paired stamps beyond +-REAC_ETF_HW_SPAN_NS */
};

void reac_etf_hw_init(struct reac_etf_hw *s, uint64_t period_ns);

/* A frame about to be sent asks for a stamp: its launch time is remembered. */
void reac_etf_hw_expect(struct reac_etf_hw *s, uint64_t launch_ns);

/* A stamp came back: paired with the remembered launch time within half a period (which
 * is then forgotten), or counted unpaired. Returns 1 when it paired. */
int reac_etf_hw_take(struct reac_etf_hw *s, uint64_t hw_ns);

/* The figures so far. Safe from any thread. */
void reac_etf_hw_read(const struct reac_etf_hw *s, struct reac_etf_hw_summary *out);

/* ---- the socket ----------------------------------------------------------- */

/* Turn the device's TX hardware stamping on (SIOCSHWTSTAMP, the RX filter left as the
 * device has it, so a ptp4l on the same NIC keeps its own) and let the socket report
 * raw hardware stamps. No frame is stamped until it asks. 0, or -errno: -EPERM without
 * CAP_NET_ADMIN, -EOPNOTSUPP/-ERANGE from a device with no TX stamping. */
int reac_etf_hw_arm(int fd, const char *ifname);

/* The per-frame request, a cmsg written at `c` (CMSG_SPACE(sizeof(uint32_t)) bytes). */
#define REAC_ETF_HW_CMSG_SPACE (CMSG_SPACE(sizeof(uint32_t)))
void reac_etf_hw_request(struct cmsghdr *c);

/* One error-queue message's raw hardware stamp, if it carries one: 1 and `*hw_ns`, else 0.
 * The pacer's drain calls it on every message it reads. */
int reac_etf_hw_stamp_of(struct msghdr *m, uint64_t *hw_ns);

#endif /* REAC_ETF_HWSTAMP_H */
