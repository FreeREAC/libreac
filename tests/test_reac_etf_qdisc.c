// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The ETF qdisc doors: the two rtnetlink messages, byte for byte, and the errno
 * classification that tells an operator which of four different things to do.
 *
 * WHY BYTE-EXACT AND NOT "IT WORKED ONCE". A netlink message the kernel refuses
 * returns from send() exactly like one it accepts; a builder with a wrong attribute
 * length, a wrong parent or a clockid off by one fails in a way that looks like a
 * missing module, a missing capability, or nothing at all. So the golden here is not
 * this builder's own output: it is what iproute2 6.17.0 actually put on a netlink
 * socket, captured with
 *
 *   unshare -rn
 *   ip link add veth0 type veth peer name veth1; ip link set veth0 up
 *   strace -e trace=sendmsg -x -s 400 \
 *       tc qdisc add dev veth0 root etf clockid CLOCK_TAI delta 300000 skip_sock_check
 *   strace -e trace=sendmsg -x -s 400 tc qdisc del dev veth0 root
 *
 * and read off that capture into the arrays below. `tc qdisc show` then printed
 *
 *   qdisc etf 800c: root refcnt 25 clockid TAI delta 300000 offload off
 *                   deadline_mode off skip_sock_check on
 *
 * which is the settings this daemon wants, so the bytes that produced it are the
 * specification. A builder proven only against itself proves nothing.
 *
 * The handle is 0 in the request and came back as 800c:, so the kernel allocates it:
 * a builder that invented one would be asserting something tc does not do. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <reac/transport/reac_etf_qdisc.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int fails;
#define CHECK(cond, ...) do { \
	if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	               printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ifindex 7 and seq 0x6aa10be4 are the capture's own values, restated here so the
 * comparison is against the WHOLE message and not against the parts we found
 * convenient. */
#define GOLD_IFINDEX 7
#define GOLD_SEQ     0x6aa10be4u

static const uint8_t GOLD_ADD[64] = {
	/* nlmsghdr: len 64, RTM_NEWQDISC (36), REQUEST|ACK|EXCL|CREATE (0x0605) */
	0x40, 0x00, 0x00, 0x00,  0x24, 0x00, 0x05, 0x06,
	0xe4, 0x0b, 0xa1, 0x6a,  0x00, 0x00, 0x00, 0x00,
	/* tcmsg: AF_UNSPEC, pad, ifindex 7, handle 0, parent TC_H_ROOT, info 0 */
	0x00, 0x00, 0x00, 0x00,  0x07, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,  0xff, 0xff, 0xff, 0xff,
	0x00, 0x00, 0x00, 0x00,
	/* TCA_KIND (1), len 8, "etf\0" */
	0x08, 0x00, 0x01, 0x00,  0x65, 0x74, 0x66, 0x00,
	/* TCA_OPTIONS (2), len 20, nesting TCA_ETF_PARMS (1), len 16 */
	0x14, 0x00, 0x02, 0x00,  0x10, 0x00, 0x01, 0x00,
	/* tc_etf_qopt: delta 300000, clockid 11 (CLOCK_TAI), flags 4 (skip_sock_check) */
	0xe0, 0x93, 0x04, 0x00,  0x0b, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00,
};

static const uint8_t GOLD_DEL[36] = {
	/* nlmsghdr: len 36, RTM_DELQDISC (37), REQUEST|ACK (0x0005) */
	0x24, 0x00, 0x00, 0x00,  0x25, 0x00, 0x05, 0x00,
	0xe4, 0x0b, 0xa1, 0x6a,  0x00, 0x00, 0x00, 0x00,
	/* the same tcmsg, and NO attributes: `root` is the parent, nothing else is said */
	0x00, 0x00, 0x00, 0x00,  0x07, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,  0xff, 0xff, 0xff, 0xff,
	0x00, 0x00, 0x00, 0x00,
};

/* THE HARDWARE-LAUNCH FORM, captured the same way inside a namespace whose veth0 has
 * four TX queues (ifindex 4, seq 0x6afa361d):
 *
 *   tc qdisc add dev veth0 root handle 100: mqprio num_tc 2 \
 *       map 1 1 1 0 1 1 1 1 1 1 1 1 1 1 1 1 queues 1@0 3@1 hw 0
 *   tc qdisc add dev veth0 parent 100:1 etf clockid CLOCK_TAI delta 300000 offload skip_sock_check
 *
 * with `strace -e trace=sendmsg -e write=3`. The second one the veth REFUSED ("Specified
 * device does not support ETF offload"), which is the namespace's fallback: the bytes are
 * still what tc asks, and that is the specification. */
#define GOLD_HW_IFINDEX 4
#define GOLD_HW_SEQ     0x6afa361du

static const uint8_t GOLD_MQPRIO[136] = {
	/* nlmsghdr: len 136, RTM_NEWQDISC, REQUEST|ACK|EXCL|CREATE */
	0x88, 0x00, 0x00, 0x00,  0x24, 0x00, 0x05, 0x06,  0x1d, 0x36, 0xfa, 0x6a,  0x00, 0x00, 0x00, 0x00,
	/* tcmsg: AF_UNSPEC, ifindex 4, handle 100: (0x01000000), parent TC_H_ROOT, info 0 */
	0x00, 0x00, 0x00, 0x00,  0x04, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x01,  0xff, 0xff, 0xff, 0xff,
	0x00, 0x00, 0x00, 0x00,
	/* TCA_KIND, len 11, "mqprio\0", padded to 12 */
	0x0b, 0x00, 0x01, 0x00,  0x6d, 0x71, 0x70, 0x72,  0x69, 0x6f, 0x00, 0x00,
	/* TCA_OPTIONS, len 88: struct tc_mqprio_qopt itself, not nested */
	0x58, 0x00, 0x02, 0x00,
	/* num_tc 2, then prio_tc_map[16] (priority 3 -> class 0, every other -> class 1), then hw 0 */
	0x02, 0x01, 0x01, 0x01,  0x00, 0x01, 0x01, 0x01,  0x01, 0x01, 0x01, 0x01,  0x01, 0x01, 0x01, 0x01,
	0x01, 0x00,
	/* count[16] (u16): class 0 one queue, class 1 three */
	0x01, 0x00, 0x03, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
	/* offset[16] (u16): class 0 at queue 0, class 1 at queue 1 */
	0x00, 0x00, 0x01, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
	/* the qopt padded to 84 */
	0x00, 0x00,
};

static const uint8_t GOLD_OFFLOAD[64] = {
	/* nlmsghdr: len 64, RTM_NEWQDISC, REQUEST|ACK|EXCL|CREATE */
	0x40, 0x00, 0x00, 0x00,  0x24, 0x00, 0x05, 0x06,
	0x1d, 0x36, 0xfa, 0x6a,  0x00, 0x00, 0x00, 0x00,
	/* tcmsg: ifindex 4, handle 0, parent 100:1 (0x01000001) */
	0x00, 0x00, 0x00, 0x00,  0x04, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,  0x01, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x00,
	/* TCA_KIND "etf\0" */
	0x08, 0x00, 0x01, 0x00,  0x65, 0x74, 0x66, 0x00,
	/* TCA_OPTIONS nesting TCA_ETF_PARMS */
	0x14, 0x00, 0x02, 0x00,  0x10, 0x00, 0x01, 0x00,
	/* delta 300000, clockid 11, flags 6 (offload | skip_sock_check) */
	0xe0, 0x93, 0x04, 0x00,  0x0b, 0x00, 0x00, 0x00,
	0x06, 0x00, 0x00, 0x00,
};

static void diff(const uint8_t *got, const uint8_t *want, size_t n, const char *what)
{
	for (size_t i = 0; i < n; i++)
		if (got[i] != want[i]) {
			fails++;
			printf("FAIL %s byte %zu is 0x%02x, iproute2 sends 0x%02x\n",
			       what, i, got[i], want[i]);
			return;   /* one is enough to read; the rest would be noise */
		}
	printf("  %s: %zu bytes identical to what iproute2 puts on the socket\n", what, n);
}

/* ---- 1. the add message is what tc sends ---------------------------------- */
static void test_add_is_byte_exact(void)
{
	uint8_t buf[128];
	memset(buf, 0xa5, sizeof buf);   /* poison: an unwritten byte must show up */
	size_t n = reac_etf_qdisc_add_msg(buf, sizeof buf, GOLD_IFINDEX,
	                                  REAC_ETF_QDISC_DELTA_NS, GOLD_SEQ);
	CHECK(n == sizeof GOLD_ADD, "the add message is %zu bytes, tc sends %zu",
	      n, sizeof GOLD_ADD);
	if (n != sizeof GOLD_ADD)
		return;
	diff(buf, GOLD_ADD, sizeof GOLD_ADD, "RTM_NEWQDISC etf");
	CHECK(buf[sizeof GOLD_ADD] == 0xa5, "the builder wrote past its own length");
}

/* ---- 2. and so is the delete ---------------------------------------------- */
static void test_del_is_byte_exact(void)
{
	uint8_t buf[128];
	memset(buf, 0xa5, sizeof buf);
	size_t n = reac_etf_qdisc_del_msg(buf, sizeof buf, GOLD_IFINDEX, GOLD_SEQ);
	CHECK(n == sizeof GOLD_DEL, "the del message is %zu bytes, tc sends %zu",
	      n, sizeof GOLD_DEL);
	if (n != sizeof GOLD_DEL)
		return;
	diff(buf, GOLD_DEL, sizeof GOLD_DEL, "RTM_DELQDISC root");
	CHECK(buf[sizeof GOLD_DEL] == 0xa5, "the builder wrote past its own length");
}

/* ---- 3. the delta is a parameter, and it reaches the wire ------------------ */
static void test_delta_reaches_the_message(void)
{
	uint8_t buf[128];
	size_t n = reac_etf_qdisc_add_msg(buf, sizeof buf, GOLD_IFINDEX, 80000u, GOLD_SEQ);
	CHECK(n == sizeof GOLD_ADD, "length moved with the delta");
	uint32_t delta;
	memcpy(&delta, buf + 52, sizeof delta);
	CHECK(delta == 80000u, "delta reached the message as %u, want 80000", delta);

	/* 0 means the proven default rather than a delta of zero, which the qdisc would
	 * take literally and then refuse every frame. */
	n = reac_etf_qdisc_add_msg(buf, sizeof buf, GOLD_IFINDEX, 0, GOLD_SEQ);
	CHECK(n == sizeof GOLD_ADD, "length moved with a zero delta");
	memcpy(&delta, buf + 52, sizeof delta);
	CHECK(delta == REAC_ETF_QDISC_DELTA_NS,
	      "a zero delta gave %u, want the proven default %u",
	      delta, REAC_ETF_QDISC_DELTA_NS);
	printf("  delta: carried through, and 0 means the proven %u ns\n",
	       REAC_ETF_QDISC_DELTA_NS);
}

/* ---- 4. a buffer that cannot hold it gets nothing, not a truncation -------- */
static void test_short_buffer_refuses(void)
{
	uint8_t buf[128];
	memset(buf, 0xa5, sizeof buf);
	CHECK(reac_etf_qdisc_add_msg(buf, sizeof GOLD_ADD - 1, GOLD_IFINDEX, 0, 1) == 0,
	      "a short buffer was written anyway");
	CHECK(buf[0] == 0xa5, "a refused build still touched the buffer");
	CHECK(reac_etf_qdisc_del_msg(buf, sizeof GOLD_DEL - 1, GOLD_IFINDEX, 1) == 0,
	      "a short buffer was written anyway (del)");
	CHECK(reac_etf_qdisc_add_msg(buf, sizeof buf, 0, 0, 1) == 0,
	      "ifindex 0 built a message; there is no such device");
	CHECK(reac_etf_qdisc_add_msg(NULL, 64, GOLD_IFINDEX, 0, 1) == 0,
	      "a NULL buffer built a message");
	printf("  refusals: a short buffer and a bad ifindex build nothing\n");
}

/* ---- 4b. the hardware-launch form is what tc sends, both halves ----------- */
static void test_hardware_form_is_byte_exact(void)
{
	uint8_t buf[256];
	memset(buf, 0xa5, sizeof buf);
	size_t n = reac_etf_qdisc_mqprio_msg(buf, sizeof buf, GOLD_HW_IFINDEX, GOLD_HW_SEQ);
	CHECK(n == sizeof GOLD_MQPRIO, "the mqprio message is %zu bytes, tc sends %zu",
	      n, sizeof GOLD_MQPRIO);
	if (n == sizeof GOLD_MQPRIO) {
		diff(buf, GOLD_MQPRIO, sizeof GOLD_MQPRIO, "RTM_NEWQDISC mqprio");
		CHECK(buf[n] == 0xa5, "the mqprio builder wrote past its own length");
	}
	CHECK(GOLD_MQPRIO[0x34 + 1 + REAC_ETF_QDISC_PRIO] == 0,
	      "the golden's map does not send REAC_ETF_QDISC_PRIO to class 0");

	memset(buf, 0xa5, sizeof buf);
	n = reac_etf_qdisc_offload_msg(buf, sizeof buf, GOLD_HW_IFINDEX,
	                               REAC_ETF_QDISC_DELTA_NS, GOLD_HW_SEQ);
	CHECK(n == sizeof GOLD_OFFLOAD, "the offload message is %zu bytes, tc sends %zu",
	      n, sizeof GOLD_OFFLOAD);
	if (n == sizeof GOLD_OFFLOAD) {
		diff(buf, GOLD_OFFLOAD, sizeof GOLD_OFFLOAD, "RTM_NEWQDISC etf offload");
		CHECK(buf[n] == 0xa5, "the offload builder wrote past its own length");
	}

	/* the refusals the software builders make, made here too */
	CHECK(reac_etf_qdisc_mqprio_msg(buf, sizeof GOLD_MQPRIO - 1, GOLD_HW_IFINDEX, 1) == 0,
	      "a short buffer took the mqprio message");
	CHECK(reac_etf_qdisc_offload_msg(buf, sizeof GOLD_OFFLOAD - 1, GOLD_HW_IFINDEX, 0, 1) == 0,
	      "a short buffer took the offload message");
	CHECK(reac_etf_qdisc_mqprio_msg(buf, sizeof buf, 0, 1) == 0 &&
	      reac_etf_qdisc_offload_msg(buf, sizeof buf, 0, 0, 1) == 0,
	      "ifindex 0 built a hardware-form message");
	CHECK(reac_etf_qdisc_offloaded(0) < 0, "ifindex 0 read as a device without offload, not unreadable");
}

/* ---- 5. the errno classification ------------------------------------------ *
 *
 * FOUR DIFFERENT PROBLEMS WITH FOUR DIFFERENT FIXES. EPERM is a capability, ENOENT
 * is a missing module, EOPNOTSUPP is the device, EINVAL is the parameters. Folding
 * any two together sends an operator to the wrong end of the room — which is the
 * whole reason this is classified by errno and never by the kernel's extack string. */
static void test_errno_classification(void)
{
	static const int codes[] = { EPERM, ENOENT, EOPNOTSUPP, EINVAL, ETIMEDOUT };
	const char *seen[sizeof codes / sizeof codes[0]];

	for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++) {
		seen[i] = reac_etf_qdisc_fix(-codes[i]);
		CHECK(seen[i] && seen[i][0], "errno %d has no fix sentence", codes[i]);
		/* Negative and positive are the same question asked two ways; a caller
		 * holding -errno must not get a different answer from one holding errno. */
		CHECK(!strcmp(seen[i], reac_etf_qdisc_fix(codes[i])),
		      "errno %d classifies differently by sign", codes[i]);
		for (size_t j = 0; j < i; j++)
			CHECK(strcmp(seen[i], seen[j]) != 0,
			      "errno %d and errno %d give the SAME fix — a classification "
			      "that collapses is no classification", codes[i], codes[j]);
	}
	/* The two an operator meets: name them, so the sentence is read once here. */
	printf("  EPERM  -> %s\n", reac_etf_qdisc_fix(-EPERM));
	printf("  ENOENT -> %s\n", reac_etf_qdisc_fix(-ENOENT));
	CHECK(!strcmp(reac_etf_qdisc_fix(0), "ok"), "0 is not a refusal");
	CHECK(reac_etf_qdisc_fix(-12345) != NULL, "an unknown errno gave NULL");
}

/* ---- 6. a device that is not there reads as UNREADABLE, never as ABSENT ---- */
static void test_state_of_no_device(void)
{
	char kind[32] = "poison";
	CHECK(reac_etf_qdisc_state(0, kind, sizeof kind) == REAC_ETF_QDISC_UNREADABLE,
	      "ifindex 0 answered something other than UNREADABLE");
	CHECK(kind[0] == '\0', "the root kind was not cleared on an unreadable probe");

	/* PRESENCE BEFORE ABSENCE: loopback is a device that certainly exists, so a
	 * reading of NONE from it is a reading and not a broken probe. */
	enum reac_etf_qdisc_state s = reac_etf_qdisc_state(1, kind, sizeof kind);
	CHECK(s != REAC_ETF_QDISC_UNREADABLE,
	      "the qdisc dump could not be read for ifindex 1 — every absence this "
	      "suite reports would be a broken search");
	printf("  probe: ifindex 1 root qdisc '%s' -> %s\n",
	       kind[0] ? kind : "(none)",
	       s == REAC_ETF_QDISC_PRESENT ? "etf present" :
	       s == REAC_ETF_QDISC_NONE ? "no etf" : "unreadable");
}

int main(void)
{
	printf("test_reac_etf_qdisc: the qdisc the daemon owns\n");
	test_add_is_byte_exact();
	test_del_is_byte_exact();
	test_delta_reaches_the_message();
	test_short_buffer_refuses();
	test_hardware_form_is_byte_exact();
	test_errno_classification();
	test_state_of_no_device();

	if (fails) {
		printf("test_reac_etf_qdisc: %d FAILED\n", fails);
		return 1;
	}
	printf("test_reac_etf_qdisc: all checks passed\n");
	return 0;
}
