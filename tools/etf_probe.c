// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* etf_probe — does the kernel actually honour a launch time on this interface?
 *
 * THE TEST IS A BURST THAT MUST ARRIVE AS A GRID. Every frame is submitted as fast
 * as the loop can call sendmsg — no pacing whatsoever — but each carries a
 * SCM_TXTIME launch time one slot period after the last. If the etf qdisc is doing
 * its job the frames leave on that grid and arrive spaced by the period; if the
 * launch times are being ignored they arrive back to back, in a few microseconds.
 * The two outcomes are three orders of magnitude apart, so this needs no fine
 * timing and no hardware clock to be decisive.
 *
 * It is its own control in both directions. The NEGATIVE arm (--no-etf) submits the
 * identical burst with no launch times at all and must show the back-to-back
 * arrival: an instrument that reports a grid either way is measuring nothing. And
 * the qdisc in force is printed with the verdict, because an `etf` result over a
 * `noqueue` qdisc is the silent no-op this whole backend exists to make impossible.
 *
 * Run it on a veth pair inside a network namespace (tools/etf-veth-probe.sh) —
 * never on the desk's NICs.
 *
 *   etf_probe --tx veth0 --rx veth1 [--fps 8000] [--count 200] [--lead-us 20000]
 *             [--no-etf]
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "reac_etf.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/net_tstamp.h>

#define PROBE_ETHERTYPE 0x88b5     /* IEEE local experimental 1 — never REAC's 0x8819,
                                    * so a stray probe can never be mistaken for a
                                    * master by anything that hears it */
#define FRAME_BYTES 1492

static int bind_packet_socket(const char *ifname, int *ifindex_out)
{
	int fd = socket(AF_PACKET, SOCK_RAW, htons(PROBE_ETHERTYPE));
	if (fd < 0)
		return -1;
	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
		close(fd);
		return -1;
	}
	struct sockaddr_ll sll;
	memset(&sll, 0, sizeof sll);
	sll.sll_family   = AF_PACKET;
	sll.sll_protocol = htons(PROBE_ETHERTYPE);
	sll.sll_ifindex  = ifr.ifr_ifindex;
	if (bind(fd, (struct sockaddr *)&sll, sizeof sll) < 0) {
		close(fd);
		return -1;
	}
	if (ifindex_out)
		*ifindex_out = ifr.ifr_ifindex;
	return fd;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y ? 1 : 0;
}

int main(int argc, char **argv)
{
	const char *tx_if = NULL, *rx_if = NULL;
	int fps = 8000, count = 200, lead_us = 20000, use_etf = 1;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--tx") && i + 1 < argc)        tx_if = argv[++i];
		else if (!strcmp(argv[i], "--rx") && i + 1 < argc)   rx_if = argv[++i];
		else if (!strcmp(argv[i], "--fps") && i + 1 < argc)  fps = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--count") && i + 1 < argc) count = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--lead-us") && i + 1 < argc) lead_us = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--no-etf"))               use_etf = 0;
		else { fprintf(stderr, "etf_probe: unknown argument %s\n", argv[i]); return 2; }
	}
	if (!tx_if || !rx_if) {
		fprintf(stderr, "usage: etf_probe --tx <if> --rx <if> [--fps N] [--count N] "
		                "[--lead-us N] [--no-etf]\n"
		                "Run it on a veth pair in a netns, never on a desk NIC.\n");
		return 2;
	}
	if (count < 8 || count > 20000) {
		fprintf(stderr, "etf_probe: --count must be 8..20000\n");
		return 2;
	}

	int tx_idx = 0;
	int tx = bind_packet_socket(tx_if, &tx_idx);
	if (tx < 0) { perror("etf_probe: tx socket"); return 2; }
	int rx = bind_packet_socket(rx_if, NULL);
	if (rx < 0) { perror("etf_probe: rx socket"); close(tx); return 2; }

	/* Kernel receive timestamps, so the arrival grid is measured by the kernel and
	 * not by how fast this loop gets back to recv. */
	int one = 1;
	if (setsockopt(rx, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof one) < 0) {
		perror("etf_probe: SO_TIMESTAMPNS");
		close(tx); close(rx); return 2;
	}

	char kind[32] = { 0 };
	enum reac_etf_qdisc q = reac_etf_qdisc_probe(tx_idx, kind, sizeof kind);
	printf("etf_probe: tx=%s rx=%s fps=%d count=%d lead=%d us arm=%s\n",
	       tx_if, rx_if, fps, count, lead_us, use_etf ? "etf" : "no-etf (control)");
	printf("  tx qdisc: root '%s', etf %s\n", kind[0] ? kind : "(none)",
	       q == REAC_ETF_QDISC_ETF ? "PRESENT"
	       : q == REAC_ETF_QDISC_ABSENT ? "ABSENT" : "UNREADABLE");

	int tai = reac_etf_tai_offset();
	printf("  kernel TAI offset: %d s\n", tai);

	if (use_etf) {
		enum reac_etf_refusal r = reac_etf_socket_arm(tx, tai, NULL);
		if (r != REAC_ETF_OK) {
			printf("  SO_TXTIME: REFUSED — %s\n", reac_etf_refusal_name(r));
			printf("VERDICT: the socket path is unavailable here; nothing was sent.\n");
			close(tx); close(rx);
			return 3;
		}
		printf("  SO_TXTIME: armed (CLOCK_TAI, strict mode, errors reported)\n");
	}

	/* One frame, repeated. The payload is irrelevant; only its arrival time is read. */
	static uint8_t frame[FRAME_BYTES];
	memset(frame, 0, sizeof frame);
	memset(frame, 0xFF, 6);
	frame[6] = 0x02;                       /* locally administered src */
	frame[12] = (PROBE_ETHERTYPE >> 8) & 0xFF;
	frame[13] = PROBE_ETHERTYPE & 0xFF;

	struct sockaddr_ll to;
	memset(&to, 0, sizeof to);
	to.sll_family  = AF_PACKET;
	to.sll_ifindex = tx_idx;
	to.sll_halen   = 6;
	memset(to.sll_addr, 0xFF, 6);

	struct reac_etf_grid g;
	if (reac_etf_grid_init(&g, fps, reac_etf_tai_ns() + (uint64_t)lead_us * 1000ull)
	    != REAC_ETF_OK) {
		fprintf(stderr, "etf_probe: bad --fps\n");
		close(tx); close(rx); return 2;
	}

	/* THE BURST. No sleeping anywhere in this loop: every frame is handed down as
	 * fast as sendmsg returns, and only the launch time says when it should leave. */
	uint64_t submit_start = reac_etf_tai_ns();
	int sent = 0;
	for (int i = 0; i < count; i++) {
		ssize_t r;
		if (use_etf) {
			struct iovec iov = { .iov_base = frame, .iov_len = FRAME_BYTES };
			uint8_t cbuf[REAC_ETF_CMSG_SPACE];
			struct msghdr msg;
			memset(&msg, 0, sizeof msg);
			msg.msg_name    = &to;
			msg.msg_namelen = sizeof to;
			msg.msg_iov     = &iov;
			msg.msg_iovlen  = 1;
			reac_etf_stamp(&msg, cbuf, g.launch_ns);
			r = sendmsg(tx, &msg, 0);
		} else {
			r = sendto(tx, frame, FRAME_BYTES, 0, (struct sockaddr *)&to, sizeof to);
		}
		if (r < 0) {
			printf("  send %d failed: %s\n", i, strerror(errno));
			break;
		}
		sent++;
		reac_etf_grid_advance(&g, 0);
	}
	uint64_t submit_end = reac_etf_tai_ns();
	printf("  submitted %d frames in %.3f ms (no pacing in the submit loop)\n",
	       sent, (submit_end - submit_start) / 1e6);

	/* Collect. Wait comfortably past the last launch time, then drain. */
	uint64_t drain_until = g.launch_ns + 200000000ull;   /* +200 ms */
	uint64_t *arr = calloc((size_t)count, sizeof *arr);
	if (!arr) { close(tx); close(rx); return 2; }
	int got = 0;
	while (reac_etf_tai_ns() < drain_until && got < count) {
		uint8_t buf[2048], ctl[256];
		struct iovec iov = { .iov_base = buf, .iov_len = sizeof buf };
		struct msghdr m;
		memset(&m, 0, sizeof m);
		m.msg_iov = &iov; m.msg_iovlen = 1;
		m.msg_control = ctl; m.msg_controllen = sizeof ctl;
		ssize_t n = recvmsg(rx, &m, MSG_DONTWAIT);
		if (n <= 0)
			continue;
		for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
			if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SO_TIMESTAMPNS)
				continue;
			struct timespec ts;
			memcpy(&ts, CMSG_DATA(c), sizeof ts);
			arr[got++] = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
		}
	}

	uint8_t code = 0;
	unsigned refused = reac_etf_drain_errors(tx, 64, &code);
	if (refused)
		printf("  the qdisc REFUSED %u frames (first SO_EE_CODE %u)\n", refused, code);

	printf("  received %d of %d\n", got, sent);
	if (got < 8) {
		printf("VERDICT: too few frames arrived to say anything. NOT a pass.\n");
		free(arr); close(tx); close(rx);
		return 3;
	}

	uint64_t *iv = calloc((size_t)got, sizeof *iv);
	int n_iv = 0;
	for (int i = 1; i < got; i++)
		if (arr[i] > arr[i - 1])
			iv[n_iv++] = arr[i] - arr[i - 1];
	if (n_iv < 4) {
		printf("VERDICT: no usable intervals. NOT a pass.\n");
		free(arr); free(iv); close(tx); close(rx);
		return 3;
	}
	qsort(iv, (size_t)n_iv, sizeof *iv, cmp_u64);
	uint64_t med = iv[n_iv / 2];
	double nominal_us = 1e9 / fps / 1000.0;
	printf("  arrival intervals: n=%d  min %.1f us  p50 %.1f us  max %.1f us "
	       "(nominal %.1f us)\n", n_iv, iv[0] / 1000.0, med / 1000.0,
	       iv[n_iv - 1] / 1000.0, nominal_us);

	/* The verdict band is deliberately wide: the two outcomes are a grid at the
	 * nominal period and a burst at a few microseconds, three orders of magnitude
	 * apart. Anything needing a tighter band than this is not what is being asked. */
	int on_grid = med > (uint64_t)(nominal_us * 1000.0 * 0.5) &&
	              med < (uint64_t)(nominal_us * 1000.0 * 2.0);
	if (use_etf) {
		if (on_grid) {
			printf("VERDICT: LAUNCH TIMES HONOURED — a burst submitted in %.3f ms "
			       "arrived spread over %.1f ms on the %.1f us grid.\n",
			       (submit_end - submit_start) / 1e6,
			       (arr[got - 1] - arr[0]) / 1e6, nominal_us);
			free(arr); free(iv); close(tx); close(rx);
			return 0;
		}
		printf("VERDICT: LAUNCH TIMES IGNORED — the burst arrived as a burst "
		       "(p50 %.1f us, nominal %.1f us). SO_TXTIME was set and the kernel "
		       "did nothing with it; check the tx qdisc line above.\n",
		       med / 1000.0, nominal_us);
		free(arr); free(iv); close(tx); close(rx);
		return 1;
	}
	/* The control arm. It must NOT be on the grid, or this probe cannot tell the
	 * two apart and its positive verdict would be worthless. */
	if (on_grid) {
		printf("VERDICT: CONTROL FAILED — an UNSTAMPED burst also arrived on the "
		       "grid, so this probe cannot distinguish the two and its etf verdict "
		       "would mean nothing.\n");
		free(arr); free(iv); close(tx); close(rx);
		return 1;
	}
	printf("VERDICT: control good — an unstamped burst arrives as a burst "
	       "(p50 %.1f us vs a %.1f us grid), so a grid in the etf arm is evidence.\n",
	       med / 1000.0, nominal_us);
	free(arr); free(iv); close(tx); close(rx);
	return 0;
}
