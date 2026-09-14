// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The ETF pacing backend. See reac_etf.h for why the launch grid is exact and why
 * the clock is CLOCK_TAI; this file is the arithmetic and the two syscalls. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reac_etf.h"

#include <errno.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/pkt_sched.h>   /* TC_H_ROOT */
#include <netpacket/packet.h>  /* SOL_PACKET */
#include <sys/timex.h>
#include <linux/net_tstamp.h>
#include <linux/errqueue.h>

const char *reac_etf_refusal_name(enum reac_etf_refusal r)
{
	switch (r) {
	case REAC_ETF_OK:                   return "ok";
	case REAC_ETF_REFUSE_NO_TXTIME:     return "this kernel has no SO_TXTIME on this socket";
	case REAC_ETF_REFUSE_NO_QDISC:      return "no etf qdisc on this netdev — every launch "
	                                           "time would be stamped and then ignored";
	case REAC_ETF_REFUSE_TAI_UNSET:     return "the kernel's TAI offset is 0 — CLOCK_TAI is "
	                                           "really UTC and every launch time would be wrong";
	case REAC_ETF_REFUSE_NO_TAI_CLOCK:  return "CLOCK_TAI is unreadable";
	case REAC_ETF_REFUSE_BAD_FPS:       return "not one of the closed pace list";
	case REAC_ETF_REFUSE_BAD_LEAD:      return "the lead is outside its bounds";
	}
	return "unknown refusal";
}

/* ---- the exact launch grid ------------------------------------------------ */

enum reac_etf_refusal reac_etf_grid_init(struct reac_etf_grid *g, int fps, uint64_t base_ns)
{
	if (!g)
		return REAC_ETF_REFUSE_BAD_FPS;
	memset(g, 0, sizeof *g);
	if (fps <= 0)
		return REAC_ETF_REFUSE_BAD_FPS;
	g->fps       = (uint32_t)fps;
	g->whole     = (uint32_t)(1000000000ull / g->fps);
	g->rem       = (uint32_t)(1000000000ull % g->fps);
	g->frac      = 0;
	g->base_ns   = base_ns;
	g->launch_ns = base_ns;
	g->slot      = 0;
	return REAC_ETF_OK;
}

uint64_t reac_etf_grid_advance(struct reac_etf_grid *g, long steer_ns)
{
	if (!g)
		return 0;
	g->slot++;
	if (steer_ns > 0) {
		/* The discipline decided this slot's period. It is already correcting the
		 * grid against a reference; accumulating our own remainder underneath it
		 * would be a second correction fighting the first. The remainder is left
		 * where it is, so a return to the nominal grid resumes its phase. */
		g->launch_ns += (uint64_t)steer_ns;
		return g->launch_ns;
	}
	/* launch(n) = base + n·whole + floor(n·rem/fps), which is exactly
	 * base + floor(n·10^9/fps) because whole·fps + rem = 10^9. `rem` is below
	 * `fps` by construction, so one subtraction always brings `frac` back in range. */
	g->launch_ns += g->whole;
	g->frac      += g->rem;
	if (g->frac >= g->fps) {
		g->frac      -= g->fps;
		g->launch_ns += 1;
	}
	return g->launch_ns;
}

void reac_etf_grid_rebase(struct reac_etf_grid *g, uint64_t now_ns)
{
	if (!g)
		return;
	/* The phase moves; the remainder does not. The pacer re-bases only when the
	 * debt exceeded its budget, and it counts the slots it abandoned — that
	 * accounting is unchanged here; only the grid it re-bases is a launch grid. */
	g->base_ns   = now_ns;
	g->launch_ns = now_ns;
	g->slot      = 0;
	(void)reac_etf_grid_advance(g, 0);
}

/* ---- the qdisc ------------------------------------------------------------ */

/* One RTM_GETQDISC dump, filtered to one ifindex. Bounded in both directions: a
 * fixed number of reads and a poll timeout, so a silent netlink socket cannot hold
 * the caller. Control plane only — this runs at open, never on the slot path. */
enum reac_etf_qdisc reac_etf_qdisc_probe(int ifindex, char *kind, size_t cap)
{
	if (kind && cap)
		kind[0] = '\0';
	if (ifindex <= 0)
		return REAC_ETF_QDISC_UNREADABLE;

	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0)
		return REAC_ETF_QDISC_UNREADABLE;

	struct {
		struct nlmsghdr nh;
		struct tcmsg    tcm;
	} req;
	memset(&req, 0, sizeof req);
	req.nh.nlmsg_len   = NLMSG_LENGTH(sizeof req.tcm);
	req.nh.nlmsg_type  = RTM_GETQDISC;
	req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.nh.nlmsg_seq   = 1;
	req.tcm.tcm_family = AF_UNSPEC;
	if (send(fd, &req, req.nh.nlmsg_len, 0) < 0) {
		close(fd);
		return REAC_ETF_QDISC_UNREADABLE;
	}

	char buf[16384] __attribute__((aligned(8)));
	int found_etf = 0, saw_any = 0, done = 0, readable = 0;
	for (int i = 0; i < 64 && !done; i++) {
		struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
		if (poll(&p, 1, 200) <= 0)
			break;
		ssize_t n = recv(fd, buf, sizeof buf, 0);
		if (n <= 0)
			break;
		readable = 1;
		size_t len = (size_t)n, off = 0;
		while (len - off >= sizeof(struct nlmsghdr)) {
			const struct nlmsghdr *nh = (const struct nlmsghdr *)(buf + off);
			size_t l = nh->nlmsg_len;
			if (l < sizeof(struct nlmsghdr) || l > len - off)
				break;
			if (nh->nlmsg_type == NLMSG_DONE || nh->nlmsg_type == NLMSG_ERROR) {
				done = 1;
				break;
			}
			if (nh->nlmsg_type == RTM_NEWQDISC &&
			    l >= NLMSG_LENGTH(sizeof(struct tcmsg))) {
				const struct tcmsg *tcm =
					(const struct tcmsg *)((const char *)nh + NLMSG_HDRLEN);
				if (tcm->tcm_ifindex == ifindex) {
					saw_any = 1;
					const struct rtattr *rta =
						(const struct rtattr *)((const char *)tcm +
						                        NLMSG_ALIGN(sizeof *tcm));
					size_t rlen = l - NLMSG_LENGTH(sizeof *tcm);
					for (; RTA_OK(rta, rlen); rta = RTA_NEXT(rta, rlen)) {
						if (rta->rta_type != TCA_KIND)
							continue;
						const char *k = (const char *)RTA_DATA(rta);
						size_t klen = RTA_PAYLOAD(rta);
						if (klen && strnlen(k, klen) < klen) {
							if (!strcmp(k, "etf"))
								found_etf = 1;
							/* Name the ROOT qdisc, which is what an
							 * operator sees in `tc qdisc show`. */
							if (kind && cap && !kind[0] &&
							    tcm->tcm_parent == TC_H_ROOT) {
								strncpy(kind, k, cap - 1);
								kind[cap - 1] = '\0';
							}
						}
					}
				}
			}
			off += NLMSG_ALIGN(l);
		}
	}
	close(fd);

	if (!readable)
		return REAC_ETF_QDISC_UNREADABLE;
	if (found_etf)
		return REAC_ETF_QDISC_ETF;
	/* The dump was read and this device appeared in it with a qdisc that is not
	 * etf. A device that appeared with NO qdisc at all is still ABSENT — that is
	 * the `noqueue` case the prior art tripped over. */
	(void)saw_any;
	return REAC_ETF_QDISC_ABSENT;
}

/* ---- the lead ------------------------------------------------------------- */

enum reac_etf_refusal reac_etf_lead_check(unsigned lead_us)
{
	if (lead_us < REAC_ETF_LEAD_US_MIN || lead_us > REAC_ETF_LEAD_US_MAX)
		return REAC_ETF_REFUSE_BAD_LEAD;
	return REAC_ETF_OK;
}

/* ---- the clock ------------------------------------------------------------ */

int reac_etf_tai_offset(void)
{
	struct timex tx;
	memset(&tx, 0, sizeof tx);
	tx.modes = 0;                      /* read only; never adjust the host's clock */
	if (adjtimex(&tx) < 0)
		return -1;
	return tx.tai;
}

uint64_t reac_etf_tai_ns(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_TAI, &ts) < 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- the socket ----------------------------------------------------------- */

static int real_setopt(int fd, int level, int optname, const void *val, socklen_t len)
{
	return setsockopt(fd, level, optname, val, len);
}

enum reac_etf_refusal reac_etf_socket_arm(int fd, int tai_offset_s,
                                          reac_etf_setopt_fn setopt)
{
	/* THE CLOCK IS CHECKED BEFORE THE SOCKET. A zero offset means nothing has ever
	 * disciplined this machine's TAI, so CLOCK_TAI reads as UTC: every launch time
	 * we computed would be 37 s behind where the qdisc reads its own clock, and the
	 * qdisc would drop every frame as expired. Arming the socket first and finding
	 * out on the wire is the failure this refusal exists to prevent. */
	if (tai_offset_s <= 0)
		return REAC_ETF_REFUSE_TAI_UNSET;

	if (!setopt)
		setopt = real_setopt;

	struct sock_txtime st;
	memset(&st, 0, sizeof st);
	st.clockid = CLOCK_TAI;
	/* STRICT MODE (no SOF_TXTIME_DEADLINE_MODE): the qdisc releases AT the launch
	 * time, never before it. A slave recovers its word clock from the inter-arrival
	 * interval, so a frame released early is exactly as wrong as one released late.
	 * SOF_TXTIME_REPORT_ERRORS puts a dropped frame on the socket's error queue,
	 * where reac_etf_drain_errors can count it, instead of letting it vanish. */
	st.flags = SOF_TXTIME_REPORT_ERRORS;
	if (setopt(fd, SOL_SOCKET, SO_TXTIME, &st, sizeof st) < 0)
		return REAC_ETF_REFUSE_NO_TXTIME;
	return REAC_ETF_OK;
}

unsigned reac_etf_drain_errors(int fd, unsigned budget, uint8_t *first_code)
{
	unsigned n = 0;
	uint8_t seen = 0;
	uint8_t ctl[512];
	uint8_t junk[64];
	for (unsigned i = 0; i < budget; i++) {
		struct iovec iov = { .iov_base = junk, .iov_len = sizeof junk };
		struct msghdr m;
		memset(&m, 0, sizeof m);
		m.msg_iov        = &iov;
		m.msg_iovlen     = 1;
		m.msg_control    = ctl;
		m.msg_controllen = sizeof ctl;
		if (recvmsg(fd, &m, MSG_ERRQUEUE | MSG_DONTWAIT) < 0)
			break;                       /* EAGAIN: the queue is empty */
		for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
			/* The pacer's socket is AF_PACKET, so packet_recvmsg puts the
			 * error at SOL_PACKET/PACKET_TX_TIMESTAMP. Anything else on this
			 * queue is not a launch-time refusal. */
			if (c->cmsg_level != SOL_PACKET)
				continue;
			if (c->cmsg_len < CMSG_LEN(sizeof(struct sock_extended_err)))
				continue;
			struct sock_extended_err ee;
			memcpy(&ee, CMSG_DATA(c), sizeof ee);
			if (ee.ee_origin != SO_EE_ORIGIN_TXTIME)
				continue;
			n++;
			if (!seen)
				seen = ee.ee_code;
		}
	}
	if (first_code)
		*first_code = seen;
	return n;
}

size_t reac_etf_stamp(struct msghdr *msg, void *cmsgbuf, uint64_t launch_ns)
{
	memset(cmsgbuf, 0, REAC_ETF_CMSG_SPACE);
	msg->msg_control    = cmsgbuf;
	msg->msg_controllen = REAC_ETF_CMSG_SPACE;

	struct cmsghdr *c = CMSG_FIRSTHDR(msg);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type  = SCM_TXTIME;
	c->cmsg_len   = CMSG_LEN(sizeof launch_ns);
	memcpy(CMSG_DATA(c), &launch_ns, sizeof launch_ns);

	msg->msg_controllen = CMSG_SPACE(sizeof launch_ns);
	return msg->msg_controllen;
}
