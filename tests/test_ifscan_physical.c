// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* A REAC segment is a physical NIC or a VLAN on one (reac_ifscan.h), read from sysfs.
 *
 * Found 2026-11-15: on a host running rootless containers the daemon listened on and knocked
 * on podman0 (a bridge) and veth0..2, because every UP Ethernet interface passed the scan's
 * gate. This builds a SCRIPTED sysfs tree in a temporary directory with each shape the kernel
 * shows, and asks reac_ifscan_is_physical about each one:
 *
 *   eno1        device link                          -> physical
 *   eno1.11     DEVTYPE=vlan, lower_eno1              -> physical (a VLAN on a NIC)
 *   eno1.11.5   DEVTYPE=vlan, lower_eno1.11           -> physical (QinQ)
 *   br0         DEVTYPE=bridge, lower_eno1            -> NOT (a bridge with a NIC port is still a bridge)
 *   veth0       nothing                               -> NOT
 *   veth0.11    DEVTYPE=vlan, lower_veth0             -> NOT (a VLAN on a veth)
 *   tap0        tun_flags                             -> NOT
 *   mv0         DEVTYPE=macvlan, lower_eno1           -> NOT
 *   ghost       not in the tree                       -> NOT
 *
 * The control: the same question about eno1 BEFORE its device link exists answers NOT, so a
 * "physical" verdict is the device link's doing and not a default. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <reac/transport/reac_ifscan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __func__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static char root[256];

static void mk(const char *ifname)
{
	char p[512];
	snprintf(p, sizeof p, "%s/%s", root, ifname);
	mkdir(p, 0755);
}

static void file(const char *ifname, const char *leaf, const char *text)
{
	char p[512];
	snprintf(p, sizeof p, "%s/%s/%s", root, ifname, leaf);
	FILE *f = fopen(p, "w");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

static void link_to(const char *ifname, const char *leaf, const char *target)
{
	char p[512];
	snprintf(p, sizeof p, "%s/%s/%s", root, ifname, leaf);
	if (symlink(target, p) != 0)
		perror(p);
}

static int phys(const char *ifname, char *why, size_t cap)
{
	return reac_ifscan_is_physical(root, ifname, why, cap);
}

int main(void)
{
	snprintf(root, sizeof root, "%s/ifscan-physical.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	if (!mkdtemp(root)) {
		perror("mkdtemp");
		return 2;
	}
	char why[200];

	mk("eno1");
	file("eno1", "uevent", "INTERFACE=eno1\nIFINDEX=2\n");
	/* CONTROL: no device link yet -> not physical. */
	CHECK(!phys("eno1", why, sizeof why), "eno1 without a device link read as physical");
	link_to("eno1", "device", "../../../devices/pci0000:00/0000:00:1f.6");
	CHECK(phys("eno1", why, sizeof why), "a NIC with a device link: %s", why);

	mk("eno1.11");
	file("eno1.11", "uevent", "DEVTYPE=vlan\nINTERFACE=eno1.11\n");
	link_to("eno1.11", "lower_eno1", "../eno1");
	CHECK(phys("eno1.11", why, sizeof why), "a VLAN on a NIC: %s", why);

	mk("eno1.11.5");
	file("eno1.11.5", "uevent", "DEVTYPE=vlan\n");
	link_to("eno1.11.5", "lower_eno1.11", "../eno1.11");
	CHECK(phys("eno1.11.5", why, sizeof why), "QinQ on a NIC: %s", why);

	mk("br0");
	file("br0", "uevent", "DEVTYPE=bridge\nINTERFACE=br0\n");
	link_to("br0", "lower_eno1", "../eno1");
	CHECK(!phys("br0", why, sizeof why) && strstr(why, "bridge"),
	      "a bridge with a NIC port is a bridge: got '%s'", why);

	mk("veth0");
	file("veth0", "uevent", "INTERFACE=veth0\n");
	CHECK(!phys("veth0", why, sizeof why) && strstr(why, "no device behind it"),
	      "a veth: got '%s'", why);

	mk("veth0.11");
	file("veth0.11", "uevent", "DEVTYPE=vlan\n");
	link_to("veth0.11", "lower_veth0", "../veth0");
	CHECK(!phys("veth0.11", why, sizeof why) && strstr(why, "a VLAN on veth0"),
	      "a VLAN on a veth: got '%s'", why);

	mk("tap0");
	file("tap0", "tun_flags", "0x1002\n");
	CHECK(!phys("tap0", why, sizeof why) && strstr(why, "tun/tap"), "a tap: got '%s'", why);

	mk("mv0");
	file("mv0", "uevent", "DEVTYPE=macvlan\n");
	link_to("mv0", "lower_eno1", "../eno1");
	CHECK(!phys("mv0", why, sizeof why) && strstr(why, "macvlan"), "a macvlan: got '%s'", why);

	CHECK(!phys("ghost", why, sizeof why) && strstr(why, "not in"), "absent: got '%s'", why);
	CHECK(!reac_ifscan_is_physical(root, "", NULL, 0), "an empty name is not physical");

	if (fails) {
		printf("test_ifscan_physical: %d FAILED (tree left at %s)\n", fails, root);
		return 1;
	}
	printf("test_ifscan_physical: all pass\n");
	return 0;
}
