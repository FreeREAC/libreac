// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The hardware-launch form against a real kernel, on a device that cannot launch by
 * itself: the namespace's fallback (docs/ETF-PACING.md, "Hardware launch").
 *
 * A veth has no launch-time engine, so it refuses `etf ... offload` exactly as a NIC
 * without one does ("Specified device does not support ETF offload"). What a caller
 * needs from that refusal, each an arm:
 *
 *   0  CONTROL     the device reads as no etf and no offload before anything is asked;
 *   1  REFUSED     reac_etf_qdisc_install_offload answers -EOPNOTSUPP, the errno the
 *                  daemon's fallback turns on (and the fix sentence names the device);
 *   2  NOTHING     all or nothing: the mqprio root the install added is gone again, so the
 *     HALF-BUILT   device reads as no etf with no mqprio root;
 *   3  SOFTWARE    the software form installs on the same device and reads back PRESENT,
 *                  and reac_etf_qdisc_offloaded() says 0 (the kernel's own flags: a
 *                  software etf is not a launch the NIC owns);
 *   4  UNREADABLE  a device that is not there reads as an error, never as 0.
 *
 * The veth has four TX queues, the i226's count, so the mqprio half the install sends is
 * the one a real NIC is sent. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <reac/transport/reac_etf_qdisc.h>

#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>

#include "netns_veth.h"

#define IF_A "rqoA"
#define IF_B "rqoB"

static int fails;
#define CHECK(cond, ...) do { \
	if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	               printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static int measure(void)
{
	int idx = (int)if_nametoindex(IF_A);
	if (idx <= 0)
		return netns_skip("the veth pair did not appear");
	char root[32];

	/* 0 CONTROL */
	CHECK(reac_etf_qdisc_state(idx, root, sizeof root) == REAC_ETF_QDISC_NONE,
	      "no etf before anything is asked");
	CHECK(reac_etf_qdisc_offloaded(idx) == 0, "no offload before anything is asked (%d)",
	      reac_etf_qdisc_offloaded(idx));

	/* 1 REFUSED */
	int rc = reac_etf_qdisc_install_offload(idx, 0);
	if (rc == -ENOENT)
		return netns_skip("the kernel has no sch_etf or sch_mqprio for a veth");
	CHECK(rc == -EOPNOTSUPP, "a device with no launch engine refuses the offload with "
	      "-EOPNOTSUPP, got %d (%s)", rc, strerror(rc < 0 ? -rc : rc));
	printf("REFUSED: install_offload -> %d: %s\n", rc, reac_etf_qdisc_fix(rc));

	/* 2 NOTHING HALF-BUILT */
	enum reac_etf_qdisc_state st = reac_etf_qdisc_state(idx, root, sizeof root);
	CHECK(st == REAC_ETF_QDISC_NONE, "the refused install left an etf behind (%d)", st);
	CHECK(strcmp(root, "mqprio") != 0, "the refused install left its mqprio root behind");
	printf("NOTHING HALF-BUILT: after the refusal the root is '%s'\n", root[0] ? root : "(none)");

	/* 3 SOFTWARE */
	rc = reac_etf_qdisc_install(idx, 0);
	CHECK(rc == 0, "the software form installs where the offload was refused (rc %d)", rc);
	CHECK(reac_etf_qdisc_state(idx, NULL, 0) == REAC_ETF_QDISC_PRESENT,
	      "the software form reads back PRESENT");
	CHECK(reac_etf_qdisc_offloaded(idx) == 0,
	      "a software etf reads as offloaded (%d)", reac_etf_qdisc_offloaded(idx));
	CHECK(reac_etf_qdisc_remove(idx) == 0, "the software form is removed again");

	/* 4 UNREADABLE */
	CHECK(reac_etf_qdisc_offloaded(0) < 0, "ifindex 0 read as a device without offload");

	if (fails) {
		printf("%d check(s) failed\n", fails);
		return 1;
	}
	printf("OK: a device with no launch engine refuses the hardware form with -EOPNOTSUPP,\n"
	       "    keeps nothing of it, takes the software form, and reads as not offloaded\n");
	return 0;
}

static int setup_and_measure(void)
{
	int rc = netns_veth_up(IF_A, IF_B, 4);
	return rc ? rc : measure();
}

int main(void)
{
	return netns_main("test_etf_qdisc_offload", setup_and_measure);
}
