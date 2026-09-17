/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * topo_bind_probe — does the topology tap hear an interface it was never bound to?
 *
 * libreac #18. An AF_PACKET socket opened with a NON-ZERO protocol is live on EVERY
 * interface from `socket()` until `bind()`; reac_topo_tap_open() puts a BPF filter,
 * PACKET_AUXDATA and if_nametoindex() in between, so a few hundred microseconds of every
 * other link's 0x8819 traffic lands in the queue and reads out as evidence about the
 * parent. On the rig (2026-09-14, reac-pw 1.0.5) that made a cold-cable NIC report a
 * tagged trunk on every start, from this daemon's own masters on ANOTHER parent.
 *
 * The instrument opens the tap OVER AND OVER on `bound` while a flood of 0x8819 frames
 * runs on `foreign`, and reads whatever the tap has queued, taking each frame's ifindex
 * from the kernel (sockaddr_ll.sll_ifindex) rather than believing the socket.
 *
 * THREE NUMBERS, AND A PASS NEEDS ALL THREE:
 *   leak    frames read off the tap whose ifindex is not `bound` — must be 0
 *   witness frames the flood really put on the wire during those opens, counted at the
 *           FAR end of the foreign pair by a properly bound socket — must be > 0, or the
 *           run proved nothing (a silent flooder and a fixed library read the same zero).
 *           It is the far end because a 0x8819-bound socket never sees its own host's
 *           OUTGOING frames: witnessing on the sending side counted zero while 88 000
 *           frames were flying, on this probe's first run.
 *   control one frame sent on `bound` after the tap is open and bound, read back — must
 *           be > 0, or the tap is deaf and its zero leak means nothing either
 *
 * Both controls exist because this is an ABSENCE proof: it must be shown able to detect
 * the presence it claims is gone, in the same run.
 *
 *   topo_bind_probe <bound-if> <bound-peer> <foreign-tx> <foreign-rx> [opens]
 *
 * It never transmits on `bound-if` itself except for the single control frame, which is
 * sent on the veth PEER so it arrives inbound, exactly as a real link's frame does.
 * Needs CAP_NET_RAW and CAP_NET_ADMIN — run it under `unshare -Ur -n` on a veth pair.
 */
#include <reac/transport/reac_topo.h>

#include <errno.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>

#define REAC_ETHERTYPE 0x8819
#define FRAME_LEN      128

static void build_frame(uint8_t *f, uint8_t tag)
{
	memset(f, 0, FRAME_LEN);
	memset(f, 0xff, 6);                     /* dst: broadcast, as REAC's own are */
	f[6] = 0x02; f[7] = 0x00; f[8] = 0x00; f[9] = 0x00; f[10] = 0x00; f[11] = tag;
	f[12] = 0x88; f[13] = 0x19;             /* the ethertype the tap's BPF passes */
	f[14] = tag;
}

/* A raw sender bound to `ifname`. Returns the fd, or -1 with errno set. */
static int sender_open(const char *ifname, struct sockaddr_ll *to)
{
	unsigned idx = if_nametoindex(ifname);
	if (idx == 0)
		return -1;
	int fd = socket(AF_PACKET, SOCK_RAW, htons(REAC_ETHERTYPE));
	if (fd < 0)
		return -1;
	memset(to, 0, sizeof *to);
	to->sll_family = AF_PACKET;
	to->sll_protocol = htons(REAC_ETHERTYPE);
	to->sll_ifindex = (int)idx;
	to->sll_halen = 6;
	memset(to->sll_addr, 0xff, 6);
	return fd;
}

/* A WITNESS on `ifname`: bound before anything is sent, so what it counts is what the
 * wire really carried while the tap was being opened. */
static int witness_open(const char *ifname)
{
	unsigned idx = if_nametoindex(ifname);
	if (idx == 0)
		return -1;
	int fd = socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_ll sll;
	memset(&sll, 0, sizeof sll);
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(REAC_ETHERTYPE);
	sll.sll_ifindex = (int)idx;
	if (bind(fd, (struct sockaddr *)&sll, sizeof sll) != 0) {
		int e = errno; close(fd); errno = e; return -1;
	}
	return fd;
}

static unsigned long drain(int fd, unsigned bound_idx, unsigned long *foreign)
{
	uint8_t buf[2048];
	struct sockaddr_ll from;
	unsigned long n = 0;
	for (;;) {
		socklen_t fl = sizeof from;
		memset(&from, 0, sizeof from);
		ssize_t r = recvfrom(fd, buf, sizeof buf, MSG_DONTWAIT,
		                     (struct sockaddr *)&from, &fl);
		if (r < 0)
			break;
		n++;
		if (foreign && (unsigned)from.sll_ifindex != bound_idx)
			(*foreign)++;
	}
	return n;
}

int main(int argc, char **argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: %s <bound-if> <bound-peer> <foreign-tx> <foreign-rx> [opens]\n",
		        argv[0]);
		return 2;
	}
	const char *bound = argv[1], *bound_peer = argv[2];
	const char *foreign_tx = argv[3], *foreign_rx = argv[4];
	long opens = (argc > 5) ? strtol(argv[5], NULL, 10) : 400;

	unsigned bound_idx = if_nametoindex(bound);
	if (bound_idx == 0) {
		fprintf(stderr, "topo_bind_probe: no interface %s\n", bound);
		return 2;
	}
	int witness = witness_open(foreign_rx);
	if (witness < 0) {
		fprintf(stderr, "topo_bind_probe: witness on %s: %s (CAP_NET_RAW?)\n",
		        foreign_rx, strerror(errno));
		return 2;
	}

	/* The flood: a child hammering 0x8819 onto the OTHER link for the whole of the
	 * open loop. Its frames must never reach a tap bound to `bound`. */
	pid_t flooder = fork();
	if (flooder == 0) {
		struct sockaddr_ll to;
		int fd = sender_open(foreign_tx, &to);
		if (fd < 0)
			_exit(3);
		uint8_t frame[FRAME_LEN];
		build_frame(frame, 0xf0);
		for (;;)
			if (sendto(fd, frame, sizeof frame, 0, (struct sockaddr *)&to,
			           sizeof to) < 0 && errno != ENOBUFS && errno != EAGAIN)
				_exit(4);
	}
	if (flooder < 0) {
		perror("topo_bind_probe: fork");
		return 2;
	}
	struct timespec settle = { 0, 20 * 1000 * 1000 };
	nanosleep(&settle, NULL);

	unsigned long leak = 0, read_total = 0;
	int open_fail = 0;
	for (long i = 0; i < opens; i++) {
		struct reac_topo_tap tap;
		if (reac_topo_tap_open(&tap, bound) != 0) {
			open_fail = errno;
			break;
		}
		read_total += drain(reac_topo_tap_fd(&tap), bound_idx, &leak);
		reac_topo_tap_close(&tap);
	}
	unsigned long witnessed = drain(witness, 0, NULL);
	kill(flooder, SIGKILL);
	waitpid(flooder, NULL, 0);
	close(witness);

	if (open_fail) {
		fprintf(stderr, "topo_bind_probe: tap open on %s: %s\n",
		        bound, strerror(open_fail));
		return 2;
	}

	/* The positive control, with the flood stopped: one frame on the bound link, which
	 * the tap must read. */
	struct reac_topo_tap tap;
	unsigned long control = 0;
	if (reac_topo_tap_open(&tap, bound) != 0) {
		fprintf(stderr, "topo_bind_probe: control tap open: %s\n", strerror(errno));
		return 2;
	}
	struct sockaddr_ll to;
	int tx = sender_open(bound_peer, &to);
	if (tx < 0) {
		fprintf(stderr, "topo_bind_probe: control sender on %s: %s\n",
		        bound_peer, strerror(errno));
		reac_topo_tap_close(&tap);
		return 2;
	}
	uint8_t frame[FRAME_LEN];
	build_frame(frame, 0x0c);
	for (int i = 0; i < 8 && control == 0; i++) {
		if (sendto(tx, frame, sizeof frame, 0, (struct sockaddr *)&to, sizeof to) < 0) {
			fprintf(stderr, "topo_bind_probe: control send: %s\n", strerror(errno));
			break;
		}
		struct pollfd p = { .fd = reac_topo_tap_fd(&tap), .events = POLLIN };
		poll(&p, 1, 100);
		control += drain(reac_topo_tap_fd(&tap), bound_idx, NULL);
	}
	close(tx);
	reac_topo_tap_close(&tap);

	printf("opens=%ld read=%lu leak=%lu witness=%lu control=%lu\n",
	       opens, read_total, leak, witnessed, control);

	if (witnessed == 0) {
		fprintf(stderr, "topo_bind_probe: NOT A RESULT — the witness on %s counted no\n"
		        "  frame, so nothing was flying during the open window. A library that\n"
		        "  leaks and one that does not both read zero here.\n", foreign_rx);
		return 2;
	}
	if (control == 0) {
		fprintf(stderr, "topo_bind_probe: NOT A RESULT — the tap read nothing from its\n"
		        "  OWN interface %s after bind, so its silence proves nothing.\n", bound);
		return 2;
	}
	if (leak > 0) {
		fprintf(stderr, "topo_bind_probe: FAIL — %lu frame(s) from an interface the tap\n"
		        "  was never bound to (%s), over %ld opens. The socket is live between\n"
		        "  socket() and bind() (libreac #18).\n", leak, foreign_tx, opens);
		return 1;
	}
	printf("OK: %ld opens, %lu foreign frame(s) on the wire (witness), 0 heard; the tap\n"
	       "    reads its own link (%lu control frame(s))\n", opens, witnessed, control);
	return 0;
}
