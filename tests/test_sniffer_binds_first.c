// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* THE SNIFFER IS DEAF UNTIL IT IS BOUND — libreac #19, which is #18 in the one socket
 * that never got #18's fix.
 *
 * An AF_PACKET socket created with a NON-ZERO protocol registers its receive hook on
 * EVERY interface on the host inside socket() itself. reac_capture_open() gave the
 * protocol to socket() and bound two syscalls later, so between the two it was a sniffer
 * on every link in the machine, and whatever landed in the queue read out afterwards as
 * a frame from the interface the caller asked for.
 *
 * WHAT THAT COST, 2026-09-21 22:17:45. reac-pw opened five sniffers in one second while
 * an S-1608 mastered enp131s0 at 8000 fps. enp128s20f0u6 — a point-to-point cable with a
 * COLD S-0808 on the far end — logged `REAC heard — box 00:40:ab:c4:80:41 (16 ch)`: the
 * S-1608's MAC, on a wire that box is not on. Two latches downstream turned that one
 * frame into a nine-minute outage, eight inputs off the desk.
 *
 * THE INSTRUMENT. Two veth pairs in a fresh user+net namespace (no root: `unshare -Ur -n`
 * holds CAP_NET_ADMIN and CAP_NET_RAW only inside it). A flood of 0x8819 frames runs on
 * one pair; reac_capture_open() is opened over and over on the OTHER, and each frame it
 * queues is read with recvfrom() so the ifindex comes from the KERNEL (sockaddr_ll) and
 * not from what the socket was asked for.
 *
 * THREE NUMBERS, AND A VERDICT NEEDS ALL THREE (topo_bind_probe's law, one socket along):
 *   leak    frames queued whose ifindex is not the bound one — must be 0
 *   witness frames the flood really put on the wire during the opens, counted at the FAR
 *           end of the foreign pair by a properly bound socket — must be > 0, or a
 *           library that leaks and one that does not both read zero here
 *   control one frame sent on the bound link after the capture is open, read back — must
 *           be > 0, or the capture is deaf and its silence proves nothing
 *
 * IT SKIPS OUT LOUD OR IT MEASURES. A missing `ip`, a kernel with no user namespaces and
 * a container that cannot make a veth are each reported as what they are — nothing was
 * tested — never as a pass about the library. Once the namespace stands, a witness or a
 * control that reads zero is RED (exit 2): the instrument got that far and lied.
 *
 * Red on the code this test was written against (2026-09-21, protocol at socket()):
 *   opens=400 read=4067 leak=4067 witness=1586 control=1
 *   FAIL — 4067 frame(s) from an interface the capture was never bound to
 * Green after the fix, same machine, same flood: leak=0 with the witness and the control
 * both still above zero.
 */
#define _GNU_SOURCE
#include <reac/reac_capture.h>

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>

#define REAC_ETHERTYPE 0x8819
#define FRAME_LEN      128

/* The bound pair and the foreign pair. Short enough for IFNAMSIZ, and they only ever
 * exist inside the namespace this process makes. */
#define IF_BOUND      "rsniffA"
#define IF_BOUND_PEER "rsniffAp"
#define IF_FOREIGN    "rsniffB"
#define IF_FOREIGN_RX "rsniffBp"

#define INNER_ENV "REAC_SNIFFER_PROBE_INNER"

static void build_frame(uint8_t *f, uint8_t tag)
{
	memset(f, 0, FRAME_LEN);
	memset(f, 0xff, 6);                     /* dst: broadcast, as REAC's own are */
	f[6] = 0x02; f[11] = tag;               /* locally-administered src */
	f[12] = 0x88; f[13] = 0x19;             /* the ethertype the sniffer asks for */
	f[14] = tag;
}

/* Run a command to completion, its output swallowed. Returns its exit status, or -1 if
 * it could not run at all. */
static int run(char *const argv[])
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO)
				close(devnull);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	int st = 0;
	if (waitpid(pid, &st, 0) < 0)
		return -1;
	if (!WIFEXITED(st))
		return -1;
	return WEXITSTATUS(st);
}

static int ip_link(char *const argv[])
{
	return run(argv);
}

/* A raw sender on `ifname`, for the flood and for the control frame. This file opens its
 * own sockets by hand rather than through the library helper on purpose: it is the
 * instrument, and an instrument that shares the code under test cannot see it break. */
static int sender_open(const char *ifname, struct sockaddr_ll *to)
{
	unsigned idx = if_nametoindex(ifname);
	if (idx == 0)
		return -1;
	int fd = socket(AF_PACKET, SOCK_RAW, 0);
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

/* The witness: bound before anything is sent, so what it counts is what the wire really
 * carried while the captures were being opened. */
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
		int e = errno;
		close(fd);
		errno = e;
		return -1;
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

static int skip(const char *why)
{
	printf("SKIPPED: test_sniffer_binds_first — %s.\n", why);
	printf("  NOTHING WAS TESTED. This is a missing capability in this environment,\n"
	       "  never a verdict about reac_capture_open(). Run it on a host shell.\n");
	return 0;
}

static int measure(long opens)
{
	unsigned bound_idx = if_nametoindex(IF_BOUND);
	if (bound_idx == 0) {
		fprintf(stderr, "test_sniffer_binds_first: no %s after setup\n", IF_BOUND);
		return 2;
	}
	int witness = witness_open(IF_FOREIGN_RX);
	if (witness < 0) {
		fprintf(stderr, "test_sniffer_binds_first: witness on %s: %s\n",
		        IF_FOREIGN_RX, strerror(errno));
		return 2;
	}

	/* The flood: a child hammering 0x8819 onto the OTHER pair for the whole of the
	 * open loop. Not one of its frames belongs to a capture bound to IF_BOUND. */
	pid_t flooder = fork();
	if (flooder == 0) {
		struct sockaddr_ll to;
		int fd = sender_open(IF_FOREIGN, &to);
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
		perror("test_sniffer_binds_first: fork");
		close(witness);
		return 2;
	}
	struct timespec settle = { 0, 20 * 1000 * 1000 };
	nanosleep(&settle, NULL);

	unsigned long leak = 0, read_total = 0;
	int open_fail = 0;
	for (long i = 0; i < opens; i++) {
		struct reac_capture c;
		if (reac_capture_open(&c, IF_BOUND) != 0) {
			open_fail = errno;
			break;
		}
		read_total += drain(c.fd, bound_idx, &leak);
		reac_capture_close(&c);
	}
	unsigned long witnessed = drain(witness, 0, NULL);
	kill(flooder, SIGKILL);
	waitpid(flooder, NULL, 0);
	close(witness);

	if (open_fail) {
		fprintf(stderr, "test_sniffer_binds_first: capture open on %s: %s\n",
		        IF_BOUND, strerror(open_fail));
		return 2;
	}

	/* The positive control, with the flood stopped: one frame on the bound link, sent
	 * from the veth PEER so it arrives inbound exactly as a real link's frame does. */
	struct reac_capture c;
	unsigned long control = 0;
	if (reac_capture_open(&c, IF_BOUND) != 0) {
		fprintf(stderr, "test_sniffer_binds_first: control capture open: %s\n",
		        strerror(errno));
		return 2;
	}
	struct sockaddr_ll to;
	int tx = sender_open(IF_BOUND_PEER, &to);
	if (tx < 0) {
		fprintf(stderr, "test_sniffer_binds_first: control sender on %s: %s\n",
		        IF_BOUND_PEER, strerror(errno));
		reac_capture_close(&c);
		return 2;
	}
	uint8_t frame[FRAME_LEN];
	build_frame(frame, 0x0c);
	for (int i = 0; i < 8 && control == 0; i++) {
		if (sendto(tx, frame, sizeof frame, 0, (struct sockaddr *)&to, sizeof to) < 0) {
			fprintf(stderr, "test_sniffer_binds_first: control send: %s\n",
			        strerror(errno));
			break;
		}
		struct pollfd p = { .fd = c.fd, .events = POLLIN, .revents = 0 };
		poll(&p, 1, 100);
		control += drain(c.fd, bound_idx, NULL);
	}
	close(tx);
	reac_capture_close(&c);

	printf("opens=%ld read=%lu leak=%lu witness=%lu control=%lu\n",
	       opens, read_total, leak, witnessed, control);

	if (witnessed == 0) {
		fprintf(stderr, "test_sniffer_binds_first: NOT A RESULT — the witness on %s\n"
		        "  counted no frame, so nothing was flying while the captures were\n"
		        "  opened. A library that leaks and one that does not read the same\n"
		        "  zero here.\n", IF_FOREIGN_RX);
		return 2;
	}
	if (control == 0) {
		fprintf(stderr, "test_sniffer_binds_first: NOT A RESULT — the capture read\n"
		        "  nothing from its OWN interface %s after the bind, so its silence\n"
		        "  proves nothing.\n", IF_BOUND);
		return 2;
	}
	if (leak > 0) {
		fprintf(stderr, "test_sniffer_binds_first: FAIL — %lu frame(s) from an\n"
		        "  interface the capture was never bound to (%s), over %ld opens.\n"
		        "  reac_capture_open() is live on every link between socket() and\n"
		        "  bind() (libreac #19). The protocol belongs to bind().\n",
		        leak, IF_FOREIGN, opens);
		return 1;
	}
	printf("OK: the sniffer is deaf until it is bound — %ld reac_capture_open() over a\n"
	       "    flood on another link, %lu foreign frames on the wire (witness), 0 heard;\n"
	       "    it reads its own link (%lu control frame(s))\n", opens, witnessed, control);
	return 0;
}

int main(int argc, char **argv)
{
	long opens = (argc > 1) ? strtol(argv[1], NULL, 10) : 400;

	if (getenv(INNER_ENV) != NULL) {
		char *const add_a[] = { "ip", "link", "add", "name", IF_BOUND,
		                        "type", "veth", "peer", "name", IF_BOUND_PEER, NULL };
		char *const add_b[] = { "ip", "link", "add", "name", IF_FOREIGN,
		                        "type", "veth", "peer", "name", IF_FOREIGN_RX, NULL };
		if (ip_link(add_a) != 0 || ip_link(add_b) != 0)
			return skip("this kernel/container cannot create a veth pair"
			            " (`ip link add ... type veth` failed)");
		const char *ifs[] = { IF_BOUND, IF_BOUND_PEER, IF_FOREIGN, IF_FOREIGN_RX };
		for (unsigned i = 0; i < sizeof ifs / sizeof ifs[0]; i++) {
			char *const up[] = { "ip", "link", "set", (char *)ifs[i], "up", NULL };
			if (ip_link(up) != 0)
				return skip("the veth pairs could not be brought up");
		}
		/* The namespace dies with this process and the veths with it — no cleanup
		 * path that could outlive a crash and leave interfaces behind. */
		return measure(opens);
	}

	/* Outer: can this environment give us a namespace and iproute2 at all? Each
	 * answer is reported as itself. */
	char self[4096];
	ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
	if (n <= 0)
		return skip("/proc/self/exe is unreadable, so the probe cannot re-exec itself");
	self[n] = '\0';

	char *const have_ip[] = { "ip", "-V", NULL };
	if (run(have_ip) != 0)
		return skip("no iproute2 (`ip`) in this environment");
	char *const have_ns[] = { "unshare", "-Ur", "-n", "true", NULL };
	if (run(have_ns) != 0)
		return skip("this environment cannot make a user+net namespace"
		            " (no CAP_SYS_ADMIN and user namespaces disabled)");

	char opens_arg[32];
	snprintf(opens_arg, sizeof opens_arg, "%ld", opens);
	setenv(INNER_ENV, "1", 1);
	char *const inner[] = { "unshare", "-Ur", "-n", self, opens_arg, NULL };
	execvp(inner[0], inner);
	perror("test_sniffer_binds_first: exec unshare");
	return 2;
}
