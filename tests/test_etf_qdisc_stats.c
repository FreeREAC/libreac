// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_etf_qdisc_stats_read against a kernel that really counts.
 *
 * WHAT IS MEASURED. An etf qdisc with skip_sock_check drops every frame that carries
 * no launch time: a tstamp of 0 is already in the past. So a burst of N unstamped
 * frames out of a device whose root is etf is N drops and nothing launched, and that
 * is exactly what the counters must say. The arms:
 *
 *   0  CONTROL   before the install, the dump completes and covers no etf (qdiscs 0);
 *   1  INSTALL   through the library's own door, read back as PRESENT;
 *   2  COUNTED   N unstamped frames -> drops grow by at least N (a frame the kernel
 *                sends on its own may join them) and by fewer than 2N (one qdisc summed
 *                once, never STATS2 and STATS both), packets and bytes do not grow,
 *                qdiscs is 1;
 *   3  REMOVED   the dump completes and covers no etf again.
 *
 * THE INSTRUMENT. One veth pair in a fresh user+net namespace (`unshare -Ur -n`: no
 * root, nothing outside the namespace touched). addrgenmode none keeps IPv6 from
 * putting its own frames through the qdisc. Where the namespace, iproute2, veth or
 * sch_etf is missing it exits 77 saying NOTHING WAS TESTED, and tests/run-test.sh
 * reports it as SKIP, never a pass. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <reac/transport/reac_etf_qdisc.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define IF_A      "rqsA"
#define IF_B      "rqsB"
#define N_FRAMES  64
#define INNER_ENV "REAC_ETF_QDISC_STATS_INNER"

static int fails;
#define CHECK(cond, ...) do { \
	if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	               printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static int capability_missing(const char *why)
{
	printf("SKIP: test_etf_qdisc_stats — %s.\n", why);
	printf("  NOTHING WAS TESTED (exit 77). This is a missing capability in this environment,\n"
	       "  never a verdict about reac_etf_qdisc. Run it on a host shell with iproute2,\n"
	       "  user namespaces and sch_etf.\n");
	return 77;
}

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
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* N unstamped frames out of `ifname`. Returns how many reached the device's qdisc:
 * a frame etf drops comes back from sendto as ENOBUFS, and that is still a frame the
 * qdisc saw. */
static int blast(const char *ifname, int n)
{
	unsigned idx = if_nametoindex(ifname);
	int fd = socket(AF_PACKET, SOCK_RAW, 0);
	if (idx == 0 || fd < 0)
		return -1;
	struct sockaddr_ll to;
	memset(&to, 0, sizeof to);
	to.sll_family = AF_PACKET;
	to.sll_protocol = htons(0x88b5);   /* IEEE local experimental */
	to.sll_ifindex = (int)idx;
	to.sll_halen = 6;
	memset(to.sll_addr, 0xff, 6);
	uint8_t frame[60];
	memset(frame, 0, sizeof frame);
	memset(frame, 0xff, 6);
	frame[6] = 0x02; frame[11] = 0x5a;
	frame[12] = 0x88; frame[13] = 0xb5;
	int reached = 0;
	for (int i = 0; i < n; i++)
		if (sendto(fd, frame, sizeof frame, 0, (struct sockaddr *)&to, sizeof to) > 0 ||
		    errno == ENOBUFS)
			reached++;
	close(fd);
	return reached;
}

static int measure(void)
{
	int idx = (int)if_nametoindex(IF_A);
	if (idx <= 0)
		return capability_missing("the veth pair did not appear");

	/* refusals by argument */
	struct reac_etf_qdisc_stats st;
	CHECK(reac_etf_qdisc_stats_read(0, &st) == -EINVAL, "ifindex 0 is refused");
	CHECK(reac_etf_qdisc_stats_read(idx, NULL) == -EINVAL, "a NULL out is refused");

	/* 0 CONTROL */
	memset(&st, 0xAA, sizeof st);
	int rc = reac_etf_qdisc_stats_read(idx, &st);
	CHECK(rc == 0, "the dump before the install completes (rc %d)", rc);
	CHECK(rc != 0 || st.qdiscs == 0, "no etf before the install, got qdiscs=%u", st.qdiscs);

	/* 1 INSTALL */
	rc = reac_etf_qdisc_install(idx, 0);
	if (rc == -ENOENT || rc == -EOPNOTSUPP)
		return capability_missing("the kernel has no sch_etf for a veth");
	CHECK(rc == 0, "the install is ACKed (rc %d)", rc);
	if (rc != 0)
		return 1;
	CHECK(reac_etf_qdisc_state(idx, NULL, 0) == REAC_ETF_QDISC_PRESENT,
	      "the install reads back as PRESENT");

	/* 2 COUNTED */
	struct reac_etf_qdisc_stats before, after;
	CHECK(reac_etf_qdisc_stats_read(idx, &before) == 0, "the dump under etf completes");
	int reached = blast(IF_A, N_FRAMES);
	CHECK(reached == N_FRAMES, "every frame reached the qdisc (%d of %d)", reached, N_FRAMES);
	rc = reac_etf_qdisc_stats_read(idx, &after);
	CHECK(rc == 0, "the dump after the burst completes (rc %d)", rc);
	unsigned long long drops = after.drops - before.drops;
	CHECK(after.qdiscs == 1, "one etf qdisc summed, got %u", after.qdiscs);
	CHECK(drops >= N_FRAMES, "every unstamped frame is a drop: %llu of %d", drops, N_FRAMES);
	CHECK(drops < 2ull * N_FRAMES, "and counted once, not twice: %llu for %d", drops, N_FRAMES);
	CHECK(after.packets == before.packets && after.bytes == before.bytes,
	      "nothing was launched: packets %llu -> %llu, bytes %llu -> %llu",
	      before.packets, after.packets, before.bytes, after.bytes);
	printf("COUNTED: %d unstamped frame(s) -> drops +%llu, packets +%llu, qdiscs %u\n",
	       N_FRAMES, drops, after.packets - before.packets, after.qdiscs);

	/* 3 REMOVED */
	CHECK(reac_etf_qdisc_remove(idx) == 0, "the removal is ACKed");
	memset(&st, 0xAA, sizeof st);
	rc = reac_etf_qdisc_stats_read(idx, &st);
	CHECK(rc == 0 && st.qdiscs == 0, "no etf after the removal (rc %d, qdiscs %u)", rc,
	      st.qdiscs);

	if (fails) {
		printf("%d check(s) failed\n", fails);
		return 1;
	}
	printf("OK: reac_etf_qdisc_stats_read counts every launch the etf qdisc refused, once,\n"
	    "    launches none, and reads no etf before the install or after the removal\n");
	return 0;
}

static int setup_and_measure(void)
{
	char *const add[]  = { "ip", "link", "add", IF_A, "type", "veth", "peer", "name", IF_B, NULL };
	char *const gen[]  = { "ip", "link", "set", IF_A, "addrgenmode", "none", NULL };
	char *const genb[] = { "ip", "link", "set", IF_B, "addrgenmode", "none", NULL };
	char *const upa[]  = { "ip", "link", "set", IF_A, "up", NULL };
	char *const upb[]  = { "ip", "link", "set", IF_B, "up", NULL };
	int rc = run(add);
	if (rc == 127)
		return capability_missing("`ip` (iproute2) is not installed");
	if (rc != 0)
		return capability_missing("this kernel cannot make a veth pair here");
	if (run(gen) != 0 || run(genb) != 0 || run(upa) != 0 || run(upb) != 0)
		return capability_missing("the veth pair could not be brought up");
	return measure();
}

int main(void)
{
	if (getenv(INNER_ENV) != NULL)
		return setup_and_measure();

	char self[4096];
	ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
	if (n <= 0)
		return capability_missing("/proc/self/exe is unreadable");
	self[n] = '\0';
	if (setenv(INNER_ENV, "1", 1) != 0)
		return capability_missing("setenv failed");

	char *const probe[] = { "unshare", "-Ur", "-n", "true", NULL };
	int prc = run(probe);
	if (prc == 127)
		return capability_missing("`unshare` is not installed");
	if (prc != 0)
		return capability_missing("this environment cannot make a user+net namespace");

	char *const argv[] = { "unshare", "-Ur", "-n", self, NULL };
	pid_t pid = fork();
	if (pid < 0)
		return capability_missing("fork failed");
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	int st = 0;
	if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st)) {
		printf("NOTHING WAS TESTED: the namespaced child did not exit normally\n");
		return 2;
	}
	return WEXITSTATUS(st);
}
