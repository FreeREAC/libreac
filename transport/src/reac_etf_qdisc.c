// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The ETF qdisc doors. See reac/transport/reac_etf_qdisc.h for whose setting this is
 * and why a leftover qdisc is as bad as a missing one; this file is the two rtnetlink
 * messages and the one ack read. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <reac/transport/reac_etf_qdisc.h>

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/pkt_sched.h>   /* TC_H_ROOT, struct tc_etf_qopt */

/* ---- what is on the device ------------------------------------------------ */

/* One RTM_GETQDISC dump, filtered to one ifindex. Bounded in both directions: a
 * fixed number of reads and a poll timeout, so a silent netlink socket cannot hold
 * the caller. Control plane only — this runs at open, never on the slot path. */
enum reac_etf_qdisc_state reac_etf_qdisc_state(int ifindex, char *kind, size_t cap)
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
		return REAC_ETF_QDISC_PRESENT;
	/* The dump was read and this device appeared in it with a qdisc that is not
	 * etf. A device that appeared with NO qdisc at all is still ABSENT — that is
	 * the `noqueue` case the prior art tripped over. */
	(void)saw_any;
	return REAC_ETF_QDISC_NONE;
}


/* ---- the messages --------------------------------------------------------- *
 *
 * The golden, from `strace -e sendmsg` on iproute2 6.17.0 inside `unshare -rn`:
 *
 *   ADD  nlmsg_len 64, RTM_NEWQDISC, REQUEST|ACK|EXCL|CREATE, pid 0
 *        tcmsg  family AF_UNSPEC, ifindex N, handle 0, parent 0xffffffff, info 0
 *        TCA_KIND    len 8   "etf\0"
 *        TCA_OPTIONS len 20  -> TCA_ETF_PARMS len 16: delta 300000, clockid 11, flags 4
 *
 *   DEL  nlmsg_len 36, RTM_DELQDISC, REQUEST|ACK, same tcmsg, no attributes.
 *
 * tcm_handle is 0 and tcm_parent is TC_H_ROOT, exactly as tc sends them: the kernel
 * allocates the handle (it came back 800c:), and `root` IS the parent. */

/* One attribute into `p`, NLA-aligned. Returns where the next one goes. */
static char *put_attr(char *p, unsigned short type, const void *val, unsigned short len)
{
	struct rtattr *rta = (struct rtattr *)p;
	rta->rta_type = type;
	rta->rta_len  = (unsigned short)RTA_LENGTH(len);
	memcpy(RTA_DATA(rta), val, len);
	return p + RTA_SPACE(len);
}

static size_t qdisc_msg(void *buf, size_t cap, int add, int ifindex,
                        uint32_t delta_ns, uint32_t seq)
{
	const size_t need = add ? REAC_ETF_QDISC_ADD_LEN : REAC_ETF_QDISC_DEL_LEN;
	if (!buf || cap < need || ifindex <= 0)
		return 0;
	memset(buf, 0, need);

	struct nlmsghdr *nh = (struct nlmsghdr *)buf;
	nh->nlmsg_type  = add ? RTM_NEWQDISC : RTM_DELQDISC;
	nh->nlmsg_flags = (unsigned short)(NLM_F_REQUEST | NLM_F_ACK |
	                                   (add ? (NLM_F_EXCL | NLM_F_CREATE) : 0));
	nh->nlmsg_seq   = seq;
	nh->nlmsg_pid   = 0;

	struct tcmsg *tcm = (struct tcmsg *)((char *)buf + NLMSG_HDRLEN);
	tcm->tcm_family  = AF_UNSPEC;
	tcm->tcm_ifindex = ifindex;
	tcm->tcm_handle  = 0;           /* the kernel allocates it, as it does for tc */
	tcm->tcm_parent  = TC_H_ROOT;
	tcm->tcm_info    = 0;

	char *p = (char *)buf + NLMSG_ALIGN(NLMSG_LENGTH(sizeof *tcm));
	if (add) {
		p = put_attr(p, TCA_KIND, "etf", 4);

		/* TCA_OPTIONS is a nest holding exactly one attribute. Built by hand
		 * rather than through a nest helper: four fields, and the golden above
		 * is the specification. */
		struct rtattr *opts = (struct rtattr *)p;
		opts->rta_type = TCA_OPTIONS;
		struct tc_etf_qopt q;
		memset(&q, 0, sizeof q);
		q.delta   = (int32_t)(delta_ns ? delta_ns : REAC_ETF_QDISC_DELTA_NS);
		q.clockid = REAC_ETF_QDISC_CLOCKID_TAI;
		q.flags   = REAC_ETF_QDISC_FLAG_SKIP_SOCK_CHECK;
		(void)put_attr((char *)RTA_DATA(opts), TCA_ETF_PARMS, &q, sizeof q);
		opts->rta_len = (unsigned short)RTA_LENGTH(RTA_SPACE(sizeof q));
		p += RTA_SPACE(RTA_SPACE(sizeof q));
	}
	nh->nlmsg_len = (uint32_t)(p - (char *)buf);
	return (size_t)nh->nlmsg_len;
}

size_t reac_etf_qdisc_add_msg(void *buf, size_t cap, int ifindex,
                              uint32_t delta_ns, uint32_t seq)
{
	return qdisc_msg(buf, cap, 1, ifindex, delta_ns, seq);
}

size_t reac_etf_qdisc_del_msg(void *buf, size_t cap, int ifindex, uint32_t seq)
{
	return qdisc_msg(buf, cap, 0, ifindex, 0, seq);
}

/* ---- the one round trip --------------------------------------------------- *
 *
 * NLM_F_ACK IS THE WHOLE POINT. A write that the kernel refuses returns the byte
 * count of the send just as a write it accepted does: counting round trips is
 * counting nothing (the mod-host proof that reported 30/30 racks and racked none).
 * So every message here is sent with ACK and the ack is READ, and its error field —
 * an errno, never a string — is what the caller is told. */
static int qdisc_send(const void *msg, size_t len)
{
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0)
		return -errno;

	struct sockaddr_nl sa;
	memset(&sa, 0, sizeof sa);
	sa.nl_family = AF_NETLINK;
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		int e = -errno;
		close(fd);
		return e;
	}
	if (send(fd, msg, len, 0) < 0) {
		int e = -errno;
		close(fd);
		return e;
	}

	/* Bounded in both directions: one poll with a timeout and one read. A control
	 * plane call that could hang would hang the daemon's start. */
	char buf[4096] __attribute__((aligned(8)));
	struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
	if (poll(&p, 1, 1000) <= 0) {
		close(fd);
		return -ETIMEDOUT;
	}
	ssize_t n = recv(fd, buf, sizeof buf, 0);
	if (n < (ssize_t)NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
		int e = n < 0 ? -errno : -EBADMSG;
		close(fd);
		return e;
	}
	close(fd);

	const struct nlmsghdr *nh = (const struct nlmsghdr *)buf;
	if (nh->nlmsg_type != NLMSG_ERROR)
		return -EBADMSG;
	const struct nlmsgerr *err = (const struct nlmsgerr *)NLMSG_DATA(nh);
	return err->error;              /* 0 is the ACK; anything else is -errno */
}

int reac_etf_qdisc_install(int ifindex, uint32_t delta_ns)
{
	char msg[REAC_ETF_QDISC_ADD_LEN];

	/* DEL THEN ADD, because etf has no change operation: `tc qdisc replace` on an
	 * existing etf root answers "Change operation not supported by specified qdisc"
	 * (measured on the rig, 2026-09-14). A -ENOENT here is the ordinary case — there
	 * was nothing to delete — and is not an error. */
	if (reac_etf_qdisc_state(ifindex, NULL, 0) == REAC_ETF_QDISC_PRESENT)
		(void)reac_etf_qdisc_remove(ifindex);

	size_t n = reac_etf_qdisc_add_msg(msg, sizeof msg, ifindex, delta_ns, 1);
	if (!n)
		return -EINVAL;
	return qdisc_send(msg, n);
}

int reac_etf_qdisc_remove(int ifindex)
{
	char msg[REAC_ETF_QDISC_DEL_LEN];
	size_t n = reac_etf_qdisc_del_msg(msg, sizeof msg, ifindex, 1);
	if (!n)
		return -EINVAL;
	return qdisc_send(msg, n);
}

const char *reac_etf_qdisc_fix(int err)
{
	/* BY ERRNO, NEVER BY THE KERNEL'S EXTACK STRING. Each of these sends the
	 * operator somewhere different, and folding two of them together sends one of
	 * them to the wrong place. */
	switch (err < 0 ? -err : err) {
	case 0:
		return "ok";
	case EPERM:
		return "this process has no CAP_NET_ADMIN — the reac-pw RPM grants it by "
		       "file capability; a daemon started by hand from a shell does not "
		       "have it";
	case ENOENT:
		return "this kernel has no sch_etf — `modprobe sch_etf`, or a kernel built "
		       "with CONFIG_NET_SCH_ETF";
	case EOPNOTSUPP:
		return "this device refuses an etf qdisc — it has no ETF hardware offload "
		       "and the request asked for one";
	case EINVAL:
		return "the etf parameters were refused — check the delta and that the "
		       "kernel accepts clockid CLOCK_TAI on this device";
	case ETIMEDOUT:
		return "rtnetlink did not answer within a second";
	default:
		return "rtnetlink refused the qdisc change";
	}
}
