// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* WHAT THE TOPOLOGY TAP HEARS ON A TRUNK, MEASURED ON A REAL KERNEL.
 *
 * Two rulings meet on this wire and this test holds both of them apart.
 *
 * THE FIRST IS SETTLED AND IS ONLY VERIFIED HERE. reac-pw's
 * docs/design/specs/2026-09-16-segments-and-roles-are-autodetected.md §1, third bullet:
 * "a tagged REAC frame heard on a trunk for a VLAN id with NO sub-interface is REPORTED by
 * its id, and the sub-interface is created". The library's half of that is reac_topo — the
 * ETH_P_ALL tap, PACKET_AUXDATA, reac_topo_classify, and the ENSURE event the binding
 * carries out. It has never been measured against a kernel that actually tags a frame:
 * test_tap.c replays a VLAN-STRIPPED pcap, and the unit arms feed reac_topo_classify() a
 * buffer this process built. A hand-built buffer cannot tell us whether the kernel
 * accelerates the tag into tp_vlan_tci or leaves it in the bytes — which is the one
 * question the classifier exists to answer. Arm A is that measurement.
 *
 * THE SECOND IS THE 2026-09-22 RULING and is what arm C is red for until it lands: "we must
 * autodetect VLANs when plugged in a switch trunk". A stagebox is a slave and says nothing
 * until a master speaks, and the master needs the netdev first — so on a cold rig NO REAC
 * frame is tagged on any VID and §1's rule can never fire (reac_declared_vlan.h carries the
 * measurement, 2026-09-15: every declared segment dead after a reboot). But the trunk is not
 * silent. A switch port carrying VLANs 11/12/13 carries their STP, LLDP and broadcast
 * traffic tagged, whatever the boxes are doing, and every one of those frames NAMES A VID.
 * Hearing one is the switch naming its VIDs — the 2026-09-19 amendment's own mechanism,
 * arrived at by listening instead of by an LLDP dialogue, and the opposite of the blind
 * 1-4094 flood that amendment rejects.
 *
 * THE ORDER OF THE ARMS IS THE DESK-SAFETY RULING, NOT A CONVENIENCE. enp131s0's native
 * VLAN is 11 and the S-4000 on it is heard UNTAGGED (measured 2026-09-10, openmixer's
 * docs/design/notes/2026-09-10-continuation-for-tecman.md). reac_topo_is_trunk() decides
 * whether that parent may be driven at all (a trunk parent is never driven: it receives every
 * sub-interface's frames untagged and would put a second master on one box). So a non-REAC
 * tag MUST NOT make a parent a trunk, or one STP frame from the switch would unserve the
 * desk's working segment. Arm C therefore runs FIRST, on a virgin table, and requires the
 * VID to be heard AND the trunk verdict to stay 0; only arm A's tagged REAC frame may set it.
 *
 * THE INSTRUMENT. One veth pair in a fresh user+net namespace (`unshare -Ur -n`: no root,
 * CAP_NET_ADMIN and CAP_NET_RAW only inside it). The tap is opened on the NEAR end, which
 * has no sub-interface of any kind — §1's exact case. The VLAN netdevs are on the FAR end,
 * so the kernel inserts the tag on egress and nothing in this file builds an 802.1Q header:
 * a test that wrote the tag itself would pass against a library that reads only the bytes
 * and fail nobody when a driver accelerates it.
 *
 * A PROBE THAT REPORTS ABSENCE MUST FIRST PROVE IT CAN DETECT PRESENCE. Arm B sends untagged
 * REAC on the parent and requires it classified; without it, arm D's silence on VID 13 and
 * arm C's silence before the fix are the same reading as a tap that never opened.
 *
 * EXIT: 0 measured and passed, 1 FAIL (the library is wrong), 2 NOTHING WAS TESTED — no
 * namespace, no iproute2, no veth, or an arm whose own control read zero. 2 is never a pass.
 *
 * Red on the code this test was written against (libreac 1.4.0, 2026-09-22, kernel
 * 7.2.6-200.fc44) — and the first measurement anywhere that §1 holds against a kernel that
 * really tags a frame:
 *   C: 5 tagged non-REAC frame(s) on vid 12 -> heard=NO frames_on_vid=0 ensure=0 is_trunk=0
 *   B: 5 untagged REAC frame(s) -> classified untagged=5 is_trunk=0 (control: the tap hears)
 *   A: 5 tagged REAC frame(s) on vid 11 -> heard=yes frames_on_vid=5 ensure=1 is_trunk=1
 *   D: 0 frame(s) on vid 13 -> heard=no (negative control)
 *   FAIL — vid 12 carried 5 tagged frame(s) past the tap and the table never heard it
 * So §1 was already true (A), and the cold VLAN was invisible (C): the BPF on the tap
 * passed 0x8819 and nothing else, so a tag on any other ethertype never reached userspace.
 *
 * WHICH FILTER ARM THIS ACTUALLY MEASURES, since two of them could admit the same frame.
 * Sabotaged both ways, same day: with the ancillary arm (SKF_AD_VLAN_TAG_PRESENT) dropped,
 * arm C reads heard=NO and this test exits 1; with the two IN-BUFFER arms (0x8100/0x88a8
 * at offset 12) dropped instead, every arm stays green. The kernel accelerates the tag on
 * veth — and on the rig's NIC (`rx-vlan-offload: on`, 2026-09-10) — so the in-buffer path
 * is not exercised by this test, by the daemon's netns tests, or by the rig. It is covered
 * only by the classifier's unit arms, which are handed bytes directly.
 */
#define _GNU_SOURCE
#include <reac/transport/reac_topo.h>

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
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
#define LLDP_ETHERTYPE 0x88cc   /* the switch's own voice, and never REAC */
#define FRAME_LEN      128

/* The trunk and its far end. Short enough that `<name>.<vid>` fits IFNAMSIZ, and they only
 * ever exist inside the namespace this process makes. */
#define IF_TRUNK  "rtopoP"
#define IF_FAR    "rtopoF"

#define VID_REAC  11   /* arm A: a box speaking tagged REAC, no sub-interface here */
#define VID_COLD  12   /* arm C: a VLAN whose box is cold — only the switch speaks */
#define VID_NEVER 13   /* arm D: carried by nothing at all */

#define INNER_ENV "REAC_TOPO_VLAN_PROBE_INNER"

static const char *PARENT = IF_TRUNK;

static int nothing_tested(const char *why)
{
	printf("NOTHING WAS TESTED: test_topo_hears_vlans — %s.\n", why);
	printf("  This is a missing capability in this environment, never a verdict about\n"
	       "  reac_topo. Run it on a host shell with iproute2 and user namespaces.\n");
	return 2;
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

static void build_frame(uint8_t *f, uint16_t ethertype, uint8_t tag)
{
	memset(f, 0, FRAME_LEN);
	memset(f, 0xff, 6);                              /* dst: broadcast, as REAC's are   */
	f[6] = 0x02; f[11] = tag;                        /* locally-administered src        */
	f[12] = (uint8_t)(ethertype >> 8);
	f[13] = (uint8_t)(ethertype & 0xff);
	f[14] = tag;
}

/* A sender on `ifname`. Sending on a VLAN netdev is how the TAG gets onto the wire: the
 * kernel inserts it on egress and this file never writes an 802.1Q header (the trunk spec's
 * fact D). Returns the fd, or -1. */
static int sender_open(const char *ifname, uint16_t ethertype, struct sockaddr_ll *to)
{
	unsigned idx = if_nametoindex(ifname);
	if (idx == 0)
		return -1;
	int fd = socket(AF_PACKET, SOCK_RAW, 0);
	if (fd < 0)
		return -1;
	memset(to, 0, sizeof *to);
	to->sll_family = AF_PACKET;
	to->sll_protocol = htons(ethertype);
	to->sll_ifindex = (int)idx;
	to->sll_halen = 6;
	memset(to->sll_addr, 0xff, 6);
	return fd;
}

/* Send `n` frames of `ethertype` out of `ifname`. Returns how many the kernel accepted. */
static int blast(const char *ifname, uint16_t ethertype, int n)
{
	struct sockaddr_ll to;
	int fd = sender_open(ifname, ethertype, &to);
	if (fd < 0)
		return -1;
	uint8_t frame[FRAME_LEN];
	build_frame(frame, ethertype, (uint8_t)n);
	int sent = 0;
	for (int i = 0; i < n; i++)
		if (sendto(fd, frame, sizeof frame, 0, (struct sockaddr *)&to, sizeof to) > 0)
			sent++;
	close(fd);
	return sent;
}

/* What one arm read off the tap. */
struct heard {
	unsigned long frames;      /* frames the tap handed back at all            */
	unsigned long untagged;    /* classified REAC_TOPO_UNTAGGED                */
	unsigned long on_vid;      /* frames carrying the VID this arm is about    */
	unsigned long reac_on_vid; /* of those, classified REAC_TOPO_TAGGED        */
	unsigned long other_on_vid;/* of those, classified REAC_TOPO_TAGGED_OTHER  */
};

/* ENSURE events, per VID, ACROSS ALL ARMS — and the arms need that, because the table and
 * its event queue are shared and a VLAN netdev is not silent just because this test has
 * sent nothing on it yet: `rtopoF.11` emits its own IPv6 multicast the moment it comes up,
 * so vid 11 is heard (and its ENSURE queued) during an earlier arm's window. An arm that
 * drained the queue and kept only its own VID's events read that ENSURE as missing, which
 * is a defect in the instrument and reads exactly like a defect in the table. */
static unsigned long ensure_seen[4096];

static unsigned long ensures(uint16_t vid)
{
	return (vid < 4096) ? ensure_seen[vid] : 0;
}

/* Drain the tap for `ms` milliseconds, feeding every frame to the table exactly as the
 * daemon's poll does. Every kind that names a VID counts, so this function needs no
 * knowledge of which kinds exist — a new one is measured the day it lands. */
static void drain(struct reac_topo_tap *tap, struct reac_topo *t, uint16_t vid_of_interest,
                  int ms, struct heard *h)
{
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		struct pollfd p = { .fd = reac_topo_tap_fd(tap), .events = POLLIN, .revents = 0 };
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long elapsed = (long)((now.tv_sec - t0.tv_sec) * 1000 +
		                      (now.tv_nsec - t0.tv_nsec) / 1000000);
		if (elapsed >= ms)
			break;
		if (poll(&p, 1, (int)(ms - elapsed)) <= 0)
			continue;
		for (;;) {
			enum reac_topo_kind kind = REAC_TOPO_NOT_REAC;
			uint16_t vid = 0;
			int r = reac_topo_tap_next(tap, &kind, &vid);
			if (r <= 0)
				break;
			h->frames++;
			uint64_t ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
			if (kind == REAC_TOPO_UNTAGGED)
				h->untagged++;
			if (vid == vid_of_interest && vid != 0) {
				h->on_vid++;
				if (kind == REAC_TOPO_TAGGED)
					h->reac_on_vid++;
				if (kind == REAC_TOPO_TAGGED_OTHER)
					h->other_on_vid++;
			}
			reac_topo_saw(t, PARENT, kind, vid, ns);
		}
	}
	struct reac_topo_event ev;
	while (reac_topo_next(t, &ev))
		if (ev.verb == REAC_TOPO_ENSURE && ev.vid < 4096)
			ensure_seen[ev.vid]++;
}

static int measure(void)
{
	struct reac_topo topo;
	struct reac_topo_tap tap;
	char cold_if[IFNAMSIZ], reac_if[IFNAMSIZ];
	int rc = 0;

	snprintf(cold_if, sizeof cold_if, "%s.%d", IF_FAR, VID_COLD);
	snprintf(reac_if, sizeof reac_if, "%s.%d", IF_FAR, VID_REAC);

	reac_topo_init(&topo);
	if (reac_topo_watch(&topo, PARENT) != 0)
		return nothing_tested("reac_topo_watch refused the parent");
	if (reac_topo_tap_open(&tap, PARENT) != 0) {
		char why[256];
		snprintf(why, sizeof why, "the tap would not open on %s (%s)",
		         PARENT, strerror(errno));
		return nothing_tested(why);
	}

	/* ---- ARM C, FIRST AND ON A VIRGIN TABLE: the switch's own voice on a cold VLAN.
	 * Nothing REAC has been heard anywhere yet, which is the cold-rig state exactly. */
	struct heard c = { 0, 0, 0, 0, 0 };
	int sent_c = blast(cold_if, LLDP_ETHERTYPE, 5);
	if (sent_c <= 0) {
		reac_topo_tap_close(&tap);
		return nothing_tested("no LLDP-shaped frame could be sent on the cold VLAN");
	}
	drain(&tap, &topo, VID_COLD, 250, &c);
	int trunk_after_c = reac_topo_is_trunk(&topo, PARENT);
	const struct reac_topo_vlan *vc = reac_topo_vlan_find(&topo, PARENT, VID_COLD);
	printf("C: %d tagged non-REAC frame(s) on vid %d -> heard=%s on_vid=%lu "
	       "classified_other=%lu ensure=%lu is_trunk=%d\n", sent_c, VID_COLD,
	       vc ? "yes" : "NO", c.on_vid, c.other_on_vid, ensures(VID_COLD), trunk_after_c);

	/* ---- ARM B: the tap can detect presence. Untagged REAC straight onto the parent's
	 * far end, which is what an access port — and the desk's NATIVE VLAN — looks like. */
	struct heard b = { 0, 0, 0, 0, 0 };
	int sent_b = blast(IF_FAR, REAC_ETHERTYPE, 5);
	if (sent_b <= 0) {
		reac_topo_tap_close(&tap);
		return nothing_tested("no untagged REAC frame could be sent on the far end");
	}
	drain(&tap, &topo, VID_NEVER, 250, &b);
	int trunk_after_b = reac_topo_is_trunk(&topo, PARENT);
	printf("B: %d untagged REAC frame(s) -> classified untagged=%lu is_trunk=%d "
	       "(control: the tap hears)\n", sent_b, b.untagged, trunk_after_b);

	/* ---- ARM A: §1 itself. Tagged REAC on a VID with no sub-interface on this side. */
	struct heard a = { 0, 0, 0, 0, 0 };
	int sent_a = blast(reac_if, REAC_ETHERTYPE, 5);
	if (sent_a <= 0) {
		reac_topo_tap_close(&tap);
		return nothing_tested("no tagged REAC frame could be sent on the REAC VLAN");
	}
	drain(&tap, &topo, VID_REAC, 250, &a);
	int trunk_after_a = reac_topo_is_trunk(&topo, PARENT);
	const struct reac_topo_vlan *va = reac_topo_vlan_find(&topo, PARENT, VID_REAC);
	printf("A: %d tagged REAC frame(s) on vid %d -> heard=%s on_vid=%lu "
	       "classified_reac=%lu ensure=%lu is_trunk=%d\n", sent_a, VID_REAC,
	       va ? "yes" : "NO", a.on_vid, a.reac_on_vid, ensures(VID_REAC), trunk_after_a);

	/* ---- ARM D: nothing was ever sent on VID_NEVER. */
	const struct reac_topo_vlan *vd = reac_topo_vlan_find(&topo, PARENT, VID_NEVER);
	printf("D: 0 frame(s) on vid %d -> heard=%s (negative control)\n",
	       VID_NEVER, vd ? "YES" : "no");

	reac_topo_tap_close(&tap);

	/* ---- THE VERDICTS. Controls first: without them every FAIL below is unreadable. */
	if (b.untagged == 0) {
		fprintf(stderr, "test_topo_hears_vlans: NOT A RESULT — the tap classified no\n"
		        "  untagged REAC frame on %s though %d were sent. It is deaf, so its\n"
		        "  silence on any VID proves nothing.\n", PARENT, sent_b);
		return 2;
	}
	if (a.reac_on_vid == 0) {
		fprintf(stderr, "test_topo_hears_vlans: FAIL — %d tagged REAC frame(s) went\n"
		        "  onto vid %d and the tap named no VID. §1's third bullet ('a tagged\n"
		        "  REAC frame ... is REPORTED by its id') does not hold in this library:\n"
		        "  either PACKET_AUXDATA is not being read or the classifier lost the\n"
		        "  tag. (2026-09-16-segments-and-roles-are-autodetected.md §1.)\n",
		        sent_a, VID_REAC);
		rc = 1;
	} else if (va == NULL || ensures(VID_REAC) == 0) {
		fprintf(stderr, "test_topo_hears_vlans: FAIL — vid %d was heard and no ENSURE\n"
		        "  was emitted, so the binding is never told to mint %s.%d.\n",
		        VID_REAC, PARENT, VID_REAC);
		rc = 1;
	}
	if (trunk_after_a != 1) {
		fprintf(stderr, "test_topo_hears_vlans: FAIL — tagged REAC was heard on %s and\n"
		        "  reac_topo_is_trunk() still says 0. A trunk parent that reads as an\n"
		        "  access port is driven, and that is two masters for one box.\n", PARENT);
		rc = 1;
	}
	if (vc == NULL || c.other_on_vid == 0 || ensures(VID_COLD) == 0) {
		fprintf(stderr, "test_topo_hears_vlans: FAIL — vid %d carried %d tagged frame(s)\n"
		        "  past the tap and the table never heard it. On a cold rig no REAC frame\n"
		        "  is ever tagged, so this is the only evidence the VLAN exists, and\n"
		        "  without it the segment is only ever reachable by a hand-written\n"
		        "  declaration (operator ruling 2026-09-22; the 2026-09-19 amendment's\n"
		        "  'the switch names its VIDs', heard rather than asked).\n",
		        VID_COLD, sent_c);
		rc = 1;
	}
	if (trunk_after_c != 0) {
		fprintf(stderr, "test_topo_hears_vlans: FAIL — a tagged frame that is NOT REAC\n"
		        "  made %s read as a trunk. The desk's native VLAN carries its box\n"
		        "  UNTAGGED (measured 2026-09-10); a trunk verdict from one STP frame\n"
		        "  would stop that parent being driven and unserve a working segment.\n",
		        PARENT);
		rc = 1;
	}
	/* The published answer the daemon reports and derives its segments from. */
	uint16_t vids[REAC_TOPO_MAX_VLANS];
	int n_vids = reac_topo_heard_vids(&topo, PARENT, vids, REAC_TOPO_MAX_VLANS);
	int saw_reac = 0, saw_cold = 0;
	for (int i = 0; i < n_vids; i++) {
		saw_reac |= (vids[i] == VID_REAC);
		saw_cold |= (vids[i] == VID_COLD);
	}
	printf("published: %d vid(s) heard on %s, %d and %d among them=%d/%d\n",
	       n_vids, PARENT, VID_REAC, VID_COLD, saw_reac, saw_cold);
	if (!saw_reac || !saw_cold) {
		fprintf(stderr, "test_topo_hears_vlans: FAIL — reac_topo_heard_vids() does not\n"
		        "  publish a VID the table holds, so a binding that asks the library\n"
		        "  which VLANs this wire carries is told less than it heard.\n");
		rc = 1;
	}
	if (vd != NULL) {
		fprintf(stderr, "test_topo_hears_vlans: FAIL — vid %d was never carried by\n"
		        "  anything and the table heard it anyway.\n", VID_NEVER);
		rc = 1;
	}
	if (rc == 0)
		printf("OK: the tap names a tagged REAC VID (§1) and a tagged non-REAC one (the\n"
		       "    cold VLAN), it hears an untagged parent, only REAC makes a trunk,\n"
		       "    and a VID nobody carried stays unheard\n");
	return rc;
}

static int setup_and_measure(void)
{
	char *const add_veth[] = { "ip", "link", "add", "name", (char *)IF_TRUNK,
	                           "type", "veth", "peer", "name", (char *)IF_FAR, NULL };
	if (run(add_veth) != 0)
		return nothing_tested("this kernel/container cannot create a veth pair"
		                      " (`ip link add ... type veth` failed)");

	/* THE PAIR COMES UP BEFORE ITS VLANS. A sub-interface whose parent is down refuses
	 * `ip link set up` with ENETDOWN, and the whole probe then reports "nothing was
	 * tested" for a reason that is purely its own. */
	const char *ifs[] = { IF_TRUNK, IF_FAR };
	for (unsigned i = 0; i < sizeof ifs / sizeof ifs[0]; i++) {
		char *const up[] = { "ip", "link", "set", (char *)ifs[i], "up", NULL };
		if (run(up) != 0)
			return nothing_tested("the veth pair could not be brought up");
	}

	/* The VLAN netdevs live on the FAR end ONLY. The near end — the one under test — has
	 * no sub-interface, which is §1's case and the whole point. */
	char vid[8], name[IFNAMSIZ];
	const int vids[] = { VID_REAC, VID_COLD };
	for (unsigned i = 0; i < sizeof vids / sizeof vids[0]; i++) {
		snprintf(vid, sizeof vid, "%d", vids[i]);
		snprintf(name, sizeof name, "%s.%d", IF_FAR, vids[i]);
		char *const add_vlan[] = { "ip", "link", "add", "link", (char *)IF_FAR,
		                           "name", name, "type", "vlan", "id", vid, NULL };
		if (run(add_vlan) != 0)
			return nothing_tested("this kernel/container cannot create an 802.1Q"
			                      " sub-interface (`ip link add ... type vlan` failed)");
		char *const up[] = { "ip", "link", "set", name, "up", NULL };
		if (run(up) != 0)
			return nothing_tested("a VLAN sub-interface could not be brought up");
	}
	/* The namespace dies with this process and every netdev in it — there is no cleanup
	 * path that could outlive a crash and leave interfaces behind. */
	return measure();
}

int main(void)
{
	if (getenv(INNER_ENV) != NULL)
		return setup_and_measure();

	char self[4096];
	ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
	if (n <= 0)
		return nothing_tested("/proc/self/exe is unreadable, so the probe cannot"
		                      " re-exec itself in a namespace");
	self[n] = '\0';
	if (setenv(INNER_ENV, "1", 1) != 0)
		return nothing_tested("setenv failed");

	char *const argv[] = { "unshare", "-Ur", "-n", self, NULL };
	pid_t pid = fork();
	if (pid < 0)
		return nothing_tested("fork failed");
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	int st = 0;
	if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st))
		return nothing_tested("the namespaced child did not exit normally");
	int code = WEXITSTATUS(st);
	if (code == 127)
		return nothing_tested("`unshare` is not installed");
	if (code == 1 || code == 0 || code == 2)
		return code;
	return nothing_tested("the namespaced child could not run"
	                      " (no user namespaces in this kernel or container?)");
}
