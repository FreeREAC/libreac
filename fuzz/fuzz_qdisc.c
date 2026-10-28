// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libFuzzer harness: THE etf QDISC READ-BACK (transport/src/reac_etf_qdisc.c), the one
 * RTM_GETQDISC dump that answers two questions — what is on the device
 * (reac_etf_qdisc_state) and what the qdisc did (reac_etf_qdisc_stats_read), the latter
 * every health window of the daemon that owns it.
 *
 * The reader is compiled with its socket calls renamed to the fake kernel of
 * fuzz/netlink.h, so every reply byte it walks is a byte of the input.
 *
 * Input:  [0] reads, mod 4; then each read:
 *           [op: bit 0 a state read (else a stats read), bit 1 socket() fails, bit 2
 *           send() fails, bit 3 a NULL out (stats) or a NULL root-kind buffer (state),
 *           bit 4 the replies end in a recv() error (else in a poll timeout), bits 5-7
 *           the root-kind capacity in threes (state)] [ifindex i8] [errno byte:
 *           socket/send] [errno byte: recv's once the replies end] [chunks: mod 5, or 60
 *           to 71 from 0xc0 up] each chunk [mode]: mode & 1 the input's own bytes
 *           (netlink.h), else [messages, mod 8] each spelled by msg_build.
 *
 * The expectations come from the bytes the fake kernel hands out, walked by the netlink
 * grammar of fuzz/netlink.h and the rules the header states
 * (include/reac/transport/reac_etf_qdisc.h:56-100): the dump is read in at most 64
 * reads, each waited for with a bounded poll; only RTM_NEWQDISC messages of the device
 * asked for count, by their first TCA_KIND when it is NUL-terminated; the state is
 * UNREADABLE when no reply arrived at all, PRESENT when an etf qdisc was seen anywhere
 * on the device, else NONE, and the root qdisc's kind is named; the counters are summed
 * over the etf qdiscs, from TCA_STATS2 when a qdisc has one, else from TCA_STATS; a
 * dump that ends in NLMSG_DONE answers 0 and the sums; one that cannot be made answers
 * -errno and leaves the caller's counters as they were.
 *
 * Properties, each an abort():
 *   - the reader closes the socket it opened, and uses no other descriptor;
 *   - it asks for one dump of every qdisc (the device is filtered on the way back);
 *   - a completed dump answers 0 and exactly the sums of the device's etf qdiscs;
 *   - a dump that fails answers its error and leaves the counters untouched;
 *   - a NULL out or a device index of 0 or less is -EINVAL and opens nothing;
 *   - the state is UNREADABLE, PRESENT or NONE as above, with the root kind named,
 *     truncated to the buffer and never written past it, and an index of 0 or less
 *     is UNREADABLE without a socket. */
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <reac/transport/reac_etf_qdisc.h>

#include "fuzz.h"
#include "netlink.h"

/* contract: linux/rtnetlink.h (RTM_NEWQDISC 36, RTM_GETQDISC 38, RTM_NEWLINK 16) and
 * linux/netlink.h (NLM_F_REQUEST 1, NLM_F_DUMP 0x300) */
#define RTM_NEWQDISC_ 36
#define RTM_GETQDISC_ 38
#define RTM_NEWLINK_  16
#define F_REQUEST     0x1
#define F_DUMP        0x300
/* contract: linux/rtnetlink.h (struct tcmsg: family, two pads, int ifindex, then handle,
 * parent, info: 20 bytes; tcm_ifindex at 4, tcm_parent at 12; TC_H_ROOT 0xffffffff) and
 * linux/pkt_sched.h, linux/gen_stats.h (TCA_KIND 1, TCA_STATS 3, TCA_STATS2 7;
 * TCA_STATS_BASIC 1, TCA_STATS_QUEUE 3) */
#define TCMSG_LEN     20
#define TC_H_ROOT_    0xffffffffu
#define TCA_KIND_     1
#define TCA_STATS_    3
#define TCA_STATS2_   7
#define ST_BASIC      1
#define ST_QUEUE      3
/* contract: linux/gen_stats.h (struct gnet_stats_basic: u64 bytes, u32 packets, 16 bytes
 * on an LP64 ABI; struct gnet_stats_queue: qlen, backlog, drops, requeues, overlimits,
 * five u32) and linux/pkt_sched.h (struct tc_stats: u64 bytes, then packets, drops,
 * overlimits, bps, pps, qlen, backlog, u32 each: 40 bytes on an LP64 ABI) */
#define BASIC_LEN     16
#define QUEUE_LEN     20
#define TC_STATS_LEN  40

#define READS_MAX     3
#define DUMP_RECVS    64   /* contract: transport/src/reac_etf_qdisc.c:61 */
#define KIND_CAP_MAX  21

/* ---- the replies the input spells --------------------------------------------------- */

/* One message of a dump. [kind] mod 8. */
static void msg_build(struct fuzz_in *in, struct nl_w *w, int ifindex)
{
	static const char *const kinds[] = { "fq_codel", "etf", "", "etfx", "et", "mq" };
	uint8_t kind = fuzz_u8(in);
	switch (kind % 8) {
	case 0: case 6: {
		/* a qdisc: of the device asked for, or another; root or a child; then
		 * attributes */
		size_t at = nl_begin(w, RTM_NEWQDISC_);
		uint8_t tcm[TCMSG_LEN] = { 0 };
		uint8_t where = fuzz_u8(in);
		int32_t idx = (where & 1) ? ifindex + 1 : ifindex;
		uint32_t parent = (where & 2) ? TC_H_ROOT_ : 0x80010000u;
		memcpy(tcm + 4, &idx, 4);
		memcpy(tcm + 12, &parent, 4);
		nl_put(w, tcm, sizeof tcm);
		int nattr = fuzz_u8(in) % 6;
		for (int i = 0; i < nattr; i++) {
			uint8_t a = fuzz_u8(in);
			switch (a % 6) {
			case 0:
				nl_attr_put(w, TCA_KIND_, "etf", 4);
				break;
			case 1: {
				/* another kind; a & 0x80 leaves its NUL off */
				const char *k = kinds[(a >> 3) % 6];
				nl_attr_put(w, TCA_KIND_, k, strlen(k) + !(a & 0x80));
				break;
			}
			case 2: {
				size_t n = nl_nest(w, TCA_STATS2_);
				int nsub = fuzz_u8(in) % 4;
				for (int k = 0; k < nsub; k++) {
					uint8_t s = fuzz_u8(in);
					uint8_t b[QUEUE_LEN] = { 0 };
					uint64_t v64 = fuzz_u32(in);
					uint32_t v1 = fuzz_u32(in), v2 = fuzz_u32(in);
					if (s % 3 == 0) {
						memcpy(b, &v64, 8);
						memcpy(b + 8, &v1, 4);
						/* s & 0x80: a payload one byte short */
						nl_attr_put(w, ST_BASIC, b, BASIC_LEN - !!(s & 0x80));
					} else if (s % 3 == 1) {
						memcpy(b + 8, &v1, 4);
						memcpy(b + 16, &v2, 4);
						nl_attr_put(w, ST_QUEUE, b, QUEUE_LEN - !!(s & 0x80));
					} else {
						nl_attr_put(w, 99, b, 4);
					}
				}
				nl_nest_end(w, n);
				break;
			}
			case 3: case 4: {
				uint8_t b[TC_STATS_LEN] = { 0 };
				uint64_t v64 = fuzz_u32(in);
				uint32_t v[3] = { fuzz_u32(in), fuzz_u32(in), fuzz_u32(in) };
				memcpy(b, &v64, 8);
				memcpy(b + 8, v, 12);
				/* 4: a TCA_STATS too short for its struct */
				nl_attr_put(w, TCA_STATS_, b, a % 6 == 3 ? TC_STATS_LEN : 20);
				break;
			}
			default:
				nl_attr_put(w, 99, "abcd", 4);
				break;
			}
		}
		nl_end(w, at);
		/* 6: the message's length is not padded, and the chunk may end there */
		if (kind % 8 == 6 && w->n == NL_ALIGN(w->n)) {
			nl_put(w, "", 1);
			nl_end(w, at);
		} else {
			nl_pad(w);
		}
		break;
	}
	case 1: {
		size_t at = nl_begin(w, NL_DONE);
		nl_put32(w, 0);
		nl_end(w, at);
		break;
	}
	case 2: {
		static const int32_t codes[] = { 0, -EPERM, -ENODEV, -EOPNOTSUPP, 5 };
		uint8_t c = fuzz_u8(in);
		nl_error(w, codes[c % 5], (c & 0x80) ? (c >> 4 & 7) : -1);
		break;
	}
	case 3: {
		/* a qdisc message too short for its tcmsg */
		size_t at = nl_begin(w, RTM_NEWQDISC_);
		nl_put32(w, 0);
		nl_put32(w, (uint32_t)ifindex);
		nl_end(w, at);
		break;
	}
	case 4: {
		size_t at = nl_begin(w, RTM_NEWLINK_);
		nl_put(w, "0123456789abcdef", 16);
		nl_end(w, at);
		break;
	}
	case 5: {
		/* a header whose length is shorter than a header */
		nl_put32(w, 8);
		nl_put32(w, NL_DONE);
		break;
	}
	default: {
		/* a DONE whose length runs past the chunk: by 4 to 64 bytes, so it may still
		 * fit the chunk as a whole, or by far more */
		size_t at = nl_begin(w, NL_DONE);
		uint8_t over = fuzz_u8(in);
		nl_set32(w, at, (over & 0x80) ? 4000 : NL_HDR + 4 * (1 + over % 16));
		break;
	}
	}
}

static void queue_chunks(struct fuzz_in *in, int ifindex)
{
	uint8_t b = fuzz_u8(in);
	int n = b >= 0xc0 ? 60 + b % 12 : b % 5;
	for (int c = 0; c < n; c++) {
		uint8_t mode = fuzz_u8(in);
		if (mode & 1) {
			nl_queue_raw(in);
			continue;
		}
		struct nl_w w = { .n = 0 };
		int m = fuzz_u8(in) % 8;
		for (int k = 0; k < m && !w.full; k++)
			msg_build(in, &w, ifindex);
		if (w.n == 0)
			nl_put32(&w, 0);   /* a chunk is never empty */
		nl_queue(&w);
	}
}

/* ---- what the reader owes ------------------------------------------------------------ */

struct expect {
	int rc;               /* 0, an error, or 1 for "an error the kernel cannot send" */
	int readable;         /* a reply arrived: the line between UNREADABLE and an answer */
	int found_etf;
	struct reac_etf_qdisc_stats sum;
	char kind[KIND_CAP_MAX]; /* the root kind, as a buffer of `cap` bytes ends up */
	size_t cap;
};

static uint64_t rd64(const uint8_t *p)
{
	uint64_t v;
	memcpy(&v, p, 8);
	return v;
}

/* One RTM_NEWQDISC message at c + off, `len` bytes: its counters into the sum, its kind
 * into the state. */
static void take_qdisc(const uint8_t *c, long off, uint32_t len, int ifindex,
                       struct expect *x)
{
	if (len < NL_HDR + TCMSG_LEN)
		return;
	if ((int32_t)nl_rd32(c + off + NL_HDR + 4) != ifindex)
		return;
	uint32_t parent = nl_rd32(c + off + NL_HDR + 12);
	long a = off + NL_HDR + NL_ALIGN(TCMSG_LEN), left = (long)len - NL_HDR - TCMSG_LEN;
	long kind = -1, st2 = -1, st1 = -1;
	uint16_t t, l;
	/* the FIRST kind attribute names the qdisc; the LAST stats attribute of each
	 * shape is the one read */
	while ((l = nl_attr(c, &a, &left, &t))) {
		if (t == TCA_KIND_ && kind < 0)
			kind = a;
		else if (t == TCA_STATS2_)
			st2 = a;
		else if (t == TCA_STATS_)
			st1 = a;
		nl_attr_next(l, &a, &left);
	}
	if (kind < 0)
		return;
	size_t kl = nl_rd16(c + kind) - RTA_HDR;
	const char *k = (const char *)c + kind + RTA_HDR;
	/* a kind is a NUL-terminated string inside its payload, else the qdisc has none */
	if (kl == 0 || !memchr(k, 0, kl))
		return;
	if (x->cap && parent == TC_H_ROOT_ && !x->kind[0]) {
		memset(x->kind, 0, x->cap);
		size_t n = strlen(k);
		memcpy(x->kind, k, n < x->cap - 1 ? n : x->cap - 1);
	}
	if (strcmp(k, "etf") != 0)
		return;
	x->found_etf = 1;
	x->sum.qdiscs++;
	if (st2 >= 0) {
		long b = st2 + RTA_HDR, bl = (long)nl_rd16(c + st2) - RTA_HDR;
		while ((l = nl_attr(c, &b, &bl, &t))) {
			const uint8_t *d = c + b + RTA_HDR;
			if (t == ST_BASIC && l - RTA_HDR >= BASIC_LEN) {
				x->sum.bytes += rd64(d);
				x->sum.packets += nl_rd32(d + 8);
			} else if (t == ST_QUEUE && l - RTA_HDR >= QUEUE_LEN) {
				x->sum.drops += nl_rd32(d + 8);
				x->sum.overlimits += nl_rd32(d + 16);
			}
			nl_attr_next(l, &b, &bl);
		}
	} else if (st1 >= 0 && nl_rd16(c + st1) - RTA_HDR >= TC_STATS_LEN) {
		const uint8_t *d = c + st1 + RTA_HDR;
		x->sum.bytes += rd64(d);
		x->sum.packets += nl_rd32(d + 8);
		x->sum.drops += nl_rd32(d + 12);
		x->sum.overlimits += nl_rd32(d + 16);
	}
}

/* The dump from the chunk `first` on, in at most DUMP_RECVS reads, and its answer. */
static void expect_dump(int first, int ifindex, struct expect *x)
{
	x->rc = 0;
	x->readable = 0;
	x->found_etf = 0;
	memset(&x->sum, 0, sizeof x->sum);
	for (int c = first;; c++) {
		if (c - first >= DUMP_RECVS) {
			x->rc = -ETIMEDOUT;
			return;
		}
		if (c >= g_nl.n_chunks) {
			x->rc = g_nl.ready_when_empty ? -g_nl.end_errno : -ETIMEDOUT;
			return;
		}
		x->readable = 1;
		const uint8_t *b = g_nl.chunk[c];
		long off = 0, left = (long)g_nl.chunk_len[c];
		uint16_t type;
		uint32_t len;
		while ((len = nl_msg(b, &off, &left, &type))) {
			if (type == NL_DONE) {
				x->rc = 0;
				return;
			}
			if (type == NL_ERROR) {
				int32_t e = len < NL_HDR + NL_ERR_BODY ? -EIO : (int32_t)nl_rd32(b + off + NL_HDR);
				/* netlink(7): an error is a negative errno, or 0 for an ACK, which
				 * answers no dump; a positive one is no reply the kernel sends, and
				 * the harness claims only that it is not a success */
				x->rc = e == 0 ? -EIO : e > 0 ? 1 : e;
				return;
			}
			if (type == RTM_NEWQDISC_)
				take_qdisc(b, off, len, ifindex, x);
			nl_msg_next(len, &off, &left);
		}
	}
}

/* The request a read owes: a 36-byte message, RTM_GETQDISC, a dump request, sequence 1,
 * then a tcmsg of family AF_UNSPEC (0) naming no device: every qdisc is asked for, and
 * the device is picked out of the replies. */
static int request_is(const uint8_t *m, size_t len)
{
	uint8_t want[NL_HDR + TCMSG_LEN] = { 0 };
	uint32_t l = sizeof want, seq = 1;
	uint16_t type = RTM_GETQDISC_, flags = F_REQUEST | F_DUMP;
	memcpy(want, &l, 4);
	memcpy(want + 4, &type, 2);
	memcpy(want + 6, &flags, 2);
	memcpy(want + 8, &seq, 4);
	return len == sizeof want && memcmp(m, want, sizeof want) == 0;
}

/* The counters as the harness filled them before the read. */
static int untouched(const struct reac_etf_qdisc_stats *out)
{
	const uint8_t *p = (const uint8_t *)out;
	for (size_t i = 0; i < sizeof *out; i++)
		if (p[i] != 0x5a)
			return 0;
	return 1;
}

/* ---- the checks ---------------------------------------------------------------------- */

static void check_socket(const struct nl_kernel *k)
{
	/* Every socket the reader opens it closes, before it returns, and it sends, polls,
	 * reads and closes only that socket.
	 * contract: include/reac/transport/reac_etf_qdisc.h:81-82 (one RTM_GETQDISC dump)
	 * mutant: transport/src/reac_etf_qdisc.c:107 `close(fd);` -> ``
	 * input: two-etf-qdiscs
	 * mutant: transport/src/reac_etf_qdisc.c:55 `close(fd);` -> ``
	 * input: send-fails */
	if (k->open || k->opened != k->closed || k->misuse)
		abort();
}

static void check_request(const struct nl_kernel *k, int sent_before)
{
	/* A read sends exactly one request, and it is a dump of every qdisc (RTM_GETQDISC,
	 * NLM_F_REQUEST | NLM_F_DUMP), a header and an empty tcmsg.
	 * contract: include/reac/transport/reac_etf_qdisc.h:81-82, linux/rtnetlink.h
	 * mutant: transport/src/reac_etf_qdisc.c:50 `NLM_F_REQUEST | NLM_F_DUMP` -> `NLM_F_REQUEST`
	 * input: two-etf-qdiscs
	 * mutant: transport/src/reac_etf_qdisc.c:51 `= 1;` -> `= 2;`
	 * input: two-etf-qdiscs
	 * mutant: transport/src/reac_etf_qdisc.c:53 `< 0) {` -> `< 0 || send(fd, &req, req.nh.nlmsg_len, 0) < 0) {`
	 * input: two-etf-qdiscs */
	if (k->n_sent != sent_before + 1 ||
	    !request_is(k->sent[sent_before], k->sent_len[sent_before]))
		abort();
}

static void check_sums(int rc, const struct reac_etf_qdisc_stats *out,
                       const struct reac_etf_qdisc_stats *want)
{
	/* A dump that reached NLMSG_DONE answers 0, and the counters are the sums over the
	 * etf qdiscs of the device: its TCA_STATS2 basic and queue blocks when it has one,
	 * else its TCA_STATS; another device's qdiscs, another kind and a kind that is not
	 * NUL-terminated count nothing, and nothing after the DONE is read.
	 * contract: include/reac/transport/reac_etf_qdisc.h:81-100
	 * mutant: transport/src/reac_etf_qdisc.c:99 `tcm->tcm_ifindex == ifindex` -> `1`
	 * input: other-device
	 * mutant: transport/src/reac_etf_qdisc.c:213 `strcmp(k, "etf") != 0` -> `strncmp(k, "etf", 2) != 0`
	 * input: other-kinds
	 * mutant: transport/src/reac_etf_qdisc.c:122 `klen && strnlen(k, klen) < klen` -> `klen`
	 * input: other-kinds
	 * mutant: transport/src/reac_etf_qdisc.c:223 `if (st2)` -> `if (0)`
	 * input: two-etf-qdiscs
	 * mutant: transport/src/reac_etf_qdisc.c:188 `o->drops      += q.drops;` -> `o->drops      = q.drops;`
	 * input: two-etf-qdiscs
	 * mutant: transport/src/reac_etf_qdisc.c:197 `RTA_PAYLOAD(st1) < sizeof(struct tc_stats)` -> `RTA_PAYLOAD(st1) < 20`
	 * input: stats1-short
	 * mutant: transport/src/reac_etf_qdisc.c:203 `o->drops      += s.drops;` -> `o->drops      += s.overlimits;`
	 * input: stats1-only
	 * mutant: transport/src/reac_etf_qdisc.c:86 `done = 1;` -> ``
	 * input: done-then-more
	 * mutant: transport/src/reac_etf_qdisc.c:85 `rc = 0;` -> ``
	 * input: two-etf-qdiscs */
	if (rc != 0 || memcmp(out, want, sizeof *out) != 0)
		abort();
}

static void check_failed(int rc, int want, const struct reac_etf_qdisc_stats *out)
{
	/* A dump that could not be made answers its error, never 0, and leaves the caller's
	 * counters as they were: unreadable is not zero. An error message too short for its
	 * code, or an ACK where a dump was asked for, is -EIO; replies that stop coming
	 * are -ETIMEDOUT, after at most 64 reads; a message whose length runs past what
	 * was read ends the walk, and so does an unpadded last message: nothing past the
	 * bytes read is ever walked (the fake kernel plants an error message there).
	 * contract: include/reac/transport/reac_etf_qdisc.h:95-99
	 * mutant: transport/src/reac_etf_qdisc.c:91 `? err->error : -EIO` -> `? err->error : 0`
	 * input: error-ack
	 * mutant: transport/src/reac_etf_qdisc.c:91 `(l >= NLMSG_LENGTH(sizeof *err) && err->error)` -> `(err->error)`
	 * input: error-short
	 * mutant: transport/src/reac_etf_qdisc.c:67 `rc = -errno;` -> `rc = 0;`
	 * input: recv-fails
	 * mutant: transport/src/reac_etf_qdisc.c:41 `return -errno;` -> `return 0;`
	 * input: socket-fails
	 * mutant: transport/src/reac_etf_qdisc.c:63 `<= 0)` -> `< 0)`
	 * input: replies-end
	 * mutant: transport/src/reac_etf_qdisc.c:61 `i < 64` -> `i < 65`
	 * input: sixty-five-chunks
	 * mutant: transport/src/reac_etf_qdisc.c:82 `l > len - off` -> `l > len`
	 * input: overlong-header
	 * mutant: transport/src/reac_etf_qdisc.c:79 `off + sizeof(struct nlmsghdr) <= len` -> `len - off >= sizeof(struct nlmsghdr)`
	 * input: unpadded-last */
	if (rc == 0 || (want != 1 && rc != want) || !untouched(out))
		abort();
}

static void check_einval(int rc, int opened_before, int opened)
{
	/* A NULL out, or an index no device has, is -EINVAL, and nothing is asked of the
	 * kernel.
	 * contract: include/reac/transport/reac_etf_qdisc.h:98-99
	 * mutant: transport/src/reac_etf_qdisc.c:231 `ifindex <= 0` -> `ifindex < 0`
	 * input: index-zero
	 * mutant: transport/src/reac_etf_qdisc.c:232 `-EINVAL` -> `-ENODEV`
	 * input: index-zero */
	if (rc != -EINVAL || opened != opened_before)
		abort();
}

static void check_state(enum reac_etf_qdisc_state got, const struct expect *x,
                        const char *kind, size_t cap)
{
	enum reac_etf_qdisc_state want = !x->readable ? REAC_ETF_QDISC_UNREADABLE :
	                                 x->found_etf ? REAC_ETF_QDISC_PRESENT :
	                                                REAC_ETF_QDISC_NONE;
	/* The state is UNREADABLE when no reply arrived at all (the socket, the send or the
	 * first read failed), PRESENT when an etf qdisc was seen anywhere on the device in
	 * the replies that did arrive, else NONE; and the root-kind buffer holds the name of
	 * the first root qdisc of the device, truncated to its capacity and NUL-terminated,
	 * or an empty string. A buffer of no capacity is not written.
	 * contract: include/reac/transport/reac_etf_qdisc.h:56-71
	 * mutant: transport/src/reac_etf_qdisc.c:162 `if (!readable)` -> `if (0)`
	 * input: state-socket-fails
	 * mutant: transport/src/reac_etf_qdisc.c:74 `*readable = 1;` -> ``
	 * input: state-present
	 * mutant: transport/src/reac_etf_qdisc.c:164 `if (w.found_etf)` -> `if (0)`
	 * input: state-present
	 * mutant: transport/src/reac_etf_qdisc.c:143 `w->found_etf = 1;` -> ``
	 * input: state-etf-under-mq
	 * mutant: transport/src/reac_etf_qdisc.c:145 `tcm->tcm_parent == TC_H_ROOT` -> `1`
	 * input: state-etf-under-mq
	 * mutant: transport/src/reac_etf_qdisc.c:147 `w->kind[w->cap - 1] = '\0';` -> ``
	 * input: state-kind-cut
	 * mutant: transport/src/reac_etf_qdisc.c:142 `if (!strcmp(k, "etf"))` -> `if (strcmp(k, "etf"))`
	 * input: state-none */
	if (got != want || (kind && memcmp(kind, x->kind, cap ? cap : 1) != 0))
		abort();
}

static void check_state_einval(enum reac_etf_qdisc_state got, const char *kind, size_t cap,
                               int opened_before, int opened)
{
	/* An index no device has is UNREADABLE, the buffer is emptied, and nothing is asked
	 * of the kernel.
	 * contract: include/reac/transport/reac_etf_qdisc.h:58-62, :67-71
	 * mutant: transport/src/reac_etf_qdisc.c:155 `ifindex <= 0` -> `ifindex < 0`
	 * input: index-zero */
	if (got != REAC_ETF_QDISC_UNREADABLE || opened != opened_before ||
	    (kind && cap && kind[0] != '\0'))
		abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size >= FUZZ_INPUT_MAX)
		return 0;
	struct fuzz_in in = fuzz_in(data, size);
	memset(&g_nl, 0, sizeof g_nl);
	int reads = fuzz_u8(&in) % (READS_MAX + 1);
	for (int r = 0; r < reads; r++) {
		uint8_t op = fuzz_u8(&in);
		int ifindex = (int8_t)fuzz_u8(&in);
		int call_errno = nl_errno(fuzz_u8(&in));
		g_nl.end_errno = nl_errno(fuzz_u8(&in));
		g_nl.socket_errno = (op & 2) ? call_errno : 0;
		g_nl.send_errno = (op & 4) ? call_errno : 0;
		g_nl.ready_when_empty = !!(op & 16);
		/* each read has its own replies, after every chunk an earlier one queued */
		int first = g_nl.n_chunks;
		queue_chunks(&in, ifindex);
		g_nl.next = first;
		int sent_before = g_nl.n_sent, opened_before = g_nl.opened;

		struct expect x;
		memset(&x, 0, sizeof x);
		if (op & 1) {
			/* ---- a state read ---- */
			size_t cap = (size_t)(op >> 5) * 3;
			/* exactly `cap` bytes, so a write past the capacity lands in a redzone;
			 * filled with a letter, so a byte the reader did not write shows */
			char *kind = (op & 8) ? NULL : malloc(cap ? cap : 1);
			/* not a property: malloc failed */
			if (!kind && !(op & 8))
				abort();
			if (kind)
				memset(kind, 'x', cap ? cap : 1);
			/* what the buffer must hold afterwards: emptied when it has a capacity,
			 * then the root kind the dump names, else untouched */
			x.cap = kind ? cap : 0;
			memset(x.kind, 'x', sizeof x.kind);
			if (x.cap)
				x.kind[0] = '\0';
			if (ifindex > 0 && !(op & 6))
				expect_dump(first, ifindex, &x);
			enum reac_etf_qdisc_state got = reac_etf_qdisc_state(ifindex, kind, cap);
			check_socket(&g_nl);
			if (ifindex <= 0) {
				check_state_einval(got, kind, cap, opened_before, g_nl.opened);
			} else {
				if (!(op & 6))
					check_request(&g_nl, sent_before);
				check_state(got, &x, kind, cap);
			}
			free(kind);
			continue;
		}
		/* ---- a stats read ---- */
		expect_dump(first, ifindex, &x);
		struct reac_etf_qdisc_stats out;
		memset(&out, 0x5a, sizeof out);
		int rc = reac_etf_qdisc_stats_read(ifindex, (op & 8) ? NULL : &out);
		check_socket(&g_nl);
		if ((op & 8) || ifindex <= 0) {
			check_einval(rc, opened_before, g_nl.opened);
		} else if (op & 6) {
			check_failed(rc, -call_errno, &out);
		} else {
			check_request(&g_nl, sent_before);
			if (x.rc == 0)
				check_sums(rc, &out, &x.sum);
			else
				check_failed(rc, x.rc, &out);
		}
	}
	return 0;
}
