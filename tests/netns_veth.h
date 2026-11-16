// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The private namespace a qdisc test runs in: one veth pair in a fresh user+net
 * namespace (`unshare -Ur -n`: no root, nothing outside the namespace touched), one
 * shape for every test that talks rtnetlink to a real kernel.
 *
 * A test calls netns_main() from main() with its own name, its inner function and the
 * veth's TX queue count. Where the namespace, iproute2 or veth is missing it exits 77
 * saying NOTHING WAS TESTED, and tests/run-test.sh reports it as SKIP, never a pass.
 * addrgenmode none keeps IPv6 from putting its own frames through the qdisc. */
#ifndef REAC_TESTS_NETNS_VETH_H
#define REAC_TESTS_NETNS_VETH_H

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define NETNS_INNER_ENV "REAC_NETNS_VETH_INNER"

static const char *netns_test_name = "test";

static int netns_skip(const char *why)
{
	printf("SKIP: %s — %s.\n", netns_test_name, why);
	printf("  NOTHING WAS TESTED (exit 77). This is a missing capability in this environment,\n"
	       "  never a verdict about the code. Run it on a host shell with iproute2,\n"
	       "  user namespaces and sch_etf.\n");
	return 77;
}

/* argv run to completion with its output discarded: its exit code, 127 when it is not
 * installed, -1 when it could not be run. */
static int netns_run(char *const argv[])
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

/* The pair a <-> b, both up, `txq` TX and RX queues each (1 for the kernel's default
 * shape). 0, or a netns_skip() exit code. */
static int netns_veth_up(const char *a, const char *b, int txq)
{
	char q[16];
	snprintf(q, sizeof q, "%d", txq > 0 ? txq : 1);
	char *const add[] = { "ip", "link", "add", (char *)a, "numtxqueues", q, "numrxqueues", q,
	                      "type", "veth", "peer", "name", (char *)b, "numtxqueues", q,
	                      "numrxqueues", q, NULL };
	char *const gen[]  = { "ip", "link", "set", (char *)a, "addrgenmode", "none", NULL };
	char *const genb[] = { "ip", "link", "set", (char *)b, "addrgenmode", "none", NULL };
	char *const upa[]  = { "ip", "link", "set", (char *)a, "up", NULL };
	char *const upb[]  = { "ip", "link", "set", (char *)b, "up", NULL };
	int rc = netns_run(add);
	if (rc == 127)
		return netns_skip("`ip` (iproute2) is not installed");
	if (rc != 0)
		return netns_skip("this kernel cannot make a veth pair here");
	if (netns_run(gen) != 0 || netns_run(genb) != 0 || netns_run(upa) != 0 || netns_run(upb) != 0)
		return netns_skip("the veth pair could not be brought up");
	return 0;
}

/* main()'s whole body: outside, re-run this binary inside `unshare -Ur -n`; inside,
 * run `inner`. Its exit code is the test's. */
static int netns_main(const char *name, int (*inner)(void))
{
	netns_test_name = name;
	if (getenv(NETNS_INNER_ENV) != NULL)
		return inner();

	char self[4096];
	ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
	if (n <= 0)
		return netns_skip("/proc/self/exe is unreadable");
	self[n] = '\0';
	if (setenv(NETNS_INNER_ENV, "1", 1) != 0)
		return netns_skip("setenv failed");

	char *const probe[] = { "unshare", "-Ur", "-n", "true", NULL };
	int prc = netns_run(probe);
	if (prc == 127)
		return netns_skip("`unshare` is not installed");
	if (prc != 0)
		return netns_skip("this environment cannot make a user+net namespace");

	char *const argv[] = { "unshare", "-Ur", "-n", self, NULL };
	pid_t pid = fork();
	if (pid < 0)
		return netns_skip("fork failed");
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

#endif /* REAC_TESTS_NETNS_VETH_H */
