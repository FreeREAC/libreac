// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* fake_box — a LINKED, SILENT REAC box on a wire, for proving a master end to
 * end without a rig.
 *
 * It behaves the way the box in `reac-captures desk-arrival-q4-2026-09-14`
 * behaves, and no more generously:
 *
 *   - it transmits NOTHING until a master puts a COMPLETE scene transfer on the
 *     wire — FIRST, 341 MIDDLEs, LAST. An interrupted push is ignored, which is
 *     the negative control the capture itself carries;
 *   - it then answers, ~8.4 ms after the last chunk, with that capture's own
 *     state-4 commit report (`cdea 01 03 0010 84`, an S-4000S-3208 declaring
 *     32 in / 8 out, chassis strap 0x00), unicast to whoever pushed;
 *   - it never cold-connects. Its `cdea 04 03` burst arrives 1.489 s after the
 *     master's ENROLL group map, exactly as measured, and not before;
 *   - once it has committed it STREAMS: 8-channel upstream filler, so a master
 *     that established sees a linked peer rather than a silence it must time out.
 *
 * Every control block is the capture's bytes. Nothing here is invented: if a
 * field is not in the capture, this program does not send it.
 *
 * Needs CAP_NET_RAW. Meant for a veth inside a private network namespace — never
 * a segment carrying a real desk, which is why it takes an interface name and
 * has no discovery of its own.
 *
 *   fake_box <ifname> [seconds]
 *
 * DROPPED MODE (`FAKE_BOX_DROPPED=1`) is the OTHER box, and the difference between
 * the two is the whole of reac-pw's wake ladder. An S-1608 that has torn its
 * master down answers no frame at all: its decompiled FSM leaves that state on
 * "PHY LINK-UP (the only establish trigger; a data gap does NOT)"
 * (reac-firmware-re REAC-PROTOCOL-FROM-SOURCE §10.2). So in this mode the program
 * COUNTS complete transfers and ignores every one of them until it has watched its
 * own carrier go away and come back — after which it behaves exactly as above.
 * The count it ignored is the measurement: it is how many whole pushes the master
 * spent on a box that could not hear them.
 */

/* AF_PACKET + SIOCGIFINDEX: built with -D_GNU_SOURCE (its own Makefile rule). */
#include <reac/reac.h>
#include <reac/reac_ctrl.h>
#include <reac/reac_ctrlblk.h>

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* The S-4000S-3208 of the capture, and its frames, verbatim. */
static const uint8_t BOX_MAC[6] = { 0x00, 0x40, 0xab, 0xc4, 0x08, 0xbc };

static const uint8_t COMMIT_REPORT[32] = {
	0x01, 0x03, 0x00, 0x10, 0x84, 0x00, 0x00, 0x00,
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
	0x01, 0x01, 0x03, 0x03, 0x00, 0x03, 0x00, 0x00,
	0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c,
};
static const uint8_t JOIN_BURST[3][32] = {
	{ 0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe,
	  0x0f, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
	  0x01, 0x00, 0x06, 0x00, 0x03, 0x00, 0x76, 0xf7,
	  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe,
	  0x0f, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
	  0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x7d, 0xf7,
	  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe,
	  0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
	  0x03, 0x02, 0x00, 0x01, 0x00, 0x7a, 0xf7, 0x00,
	  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 },
};

#define BOX_CHANNELS 8
#define COMMIT_DELAY_NS   8400000LL      /* +8.4 ms after the LAST, measured */
#define JOIN_DELAY_NS  1489000000LL      /* +1.489 s after the group map     */

/* Our own carrier, read the way reac_carrier.c reads it: the kernel's file, no
 * netlink, no capability. -1 is UNKNOWN and is never treated as down — a box that
 * mistook an unreadable probe for a link-down would wake itself and prove nothing. */
static int my_carrier(const char *ifname)
{
	char path[IFNAMSIZ + 32];
	if (snprintf(path, sizeof path, "/sys/class/net/%s/carrier", ifname) < 0)
		return -1;
	FILE *f = fopen(path, "re");
	if (!f)
		return -1;
	int c = fgetc(f);
	fclose(f);
	return c == '1' ? 1 : (c == '0' ? 0 : -1);
}

static uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static size_t build(uint8_t *out, const uint8_t dst[6], const uint8_t blk[32])
{
	size_t len = reac_ctrl_box_frame_len(BOX_CHANNELS);
	memset(out, 0, len);
	memcpy(out, dst, 6);
	memcpy(out + 6, BOX_MAC, 6);
	out[12] = 0x88; out[13] = 0x19;
	out[16] = 0xcd; out[17] = 0xea;
	memcpy(out + 18, blk, 32);
	out[len - 2] = REAC_END_MARKER_0;
	out[len - 1] = REAC_END_MARKER_1;
	reac_ctrl_checksum_apply(out);
	return len;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <ifname> [seconds]\n", argv[0]);
		return 2;
	}
	int secs = argc > 2 ? atoi(argv[2]) : 30;

	int fd = socket(AF_PACKET, SOCK_RAW, htons(0x8819));
	if (fd < 0) { perror("socket"); return 1; }
	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", argv[1]);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) { perror("SIOCGIFINDEX"); return 1; }
	int ifindex = ifr.ifr_ifindex;
	struct sockaddr_ll bind_ll;
	memset(&bind_ll, 0, sizeof bind_ll);
	bind_ll.sll_family = AF_PACKET;
	bind_ll.sll_protocol = htons(0x8819);
	bind_ll.sll_ifindex = ifindex;
	if (bind(fd, (struct sockaddr *)&bind_ll, sizeof bind_ll) < 0) {
		perror("bind"); return 1;
	}
	struct timeval tv = { 0, 20000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

	struct sockaddr_ll to;
	memset(&to, 0, sizeof to);
	to.sll_family = AF_PACKET;
	to.sll_ifindex = ifindex;
	to.sll_halen = 6;

	uint8_t rx[REAC_FRAME_BYTES + 64], tx[REAC_FRAME_BYTES];
	uint8_t master[6] = { 0 };
	int have_master = 0, saw_first = 0, mids = 0, committed = 0, joined = 0;
	long transfers = 0, ignored = 0, sent = 0;
	/* DROPPED MODE: deaf to every frame until its own PHY comes back. */
	const char *dropped_env = getenv("FAKE_BOX_DROPPED");
	const int dropped = dropped_env && dropped_env[0] == '1';
	int phy_edge = !dropped;      /* a box that never dropped needs no edge */
	int carrier_was = my_carrier(argv[1]), saw_down = 0;
	long deaf_transfers = 0;
	uint64_t commit_at = 0, join_at = 0, next_stream = 0;
	uint64_t stop = mono_ns() + (uint64_t)secs * 1000000000ull;

	while (mono_ns() < stop) {
		ssize_t n = recv(fd, rx, sizeof rx, 0);
		uint64_t now = mono_ns();
		if (n > 0) {
			struct reac_ctrl_parsed p;
			enum reac_ctrl_kind k = reac_ctrl_parse(rx, (size_t)n, &p);
			if (k != REAC_CTRL_NONE && memcmp(p.src, BOX_MAC, 6) != 0) {
				if (!have_master) {
					memcpy(master, p.src, 6);
					have_master = 1;
				}
				if (k == REAC_CTRL_SCENE_TRANSFER && p.link == REAC_LINK_CTRL &&
				    p.opcode == REAC_OP_BULK) {
					if (p.seg == REAC_SEG_FIRST) {
						saw_first = 1; mids = 0;
					} else if (p.seg == REAC_SEG_MIDDLE) {
						if (saw_first) mids++;
					} else if (p.seg == REAC_SEG_LAST) {
						if (saw_first && mids == REAC_SCENE_CHUNKS) {
							transfers++;
							/* THE WHOLE POINT OF THE MODE: a complete
							 * transfer, correctly received, and it
							 * changes nothing. */
							if (!phy_edge)
								deaf_transfers++;
							else if (!committed)
								commit_at = now + COMMIT_DELAY_NS;
						} else {
							ignored++;   /* the capture's negative control */
						}
						saw_first = 0; mids = 0;
					}
				} else if (committed && !joined && !join_at &&
				           k == REAC_CTRL_GROUP_MAP) {
					join_at = now + JOIN_DELAY_NS;
				}
			}
		}

		/* OUR OWN PHY, polled on the same ~20 ms tick the receive timeout gives
		 * us. A down we never saw cannot be followed by an up we believe in, so
		 * both halves are required and in that order. */
		if (!phy_edge) {
			int c = my_carrier(argv[1]);
			if (c == 0 && carrier_was != 0)
				saw_down = 1;
			if (c == 1 && saw_down) {
				phy_edge = 1;
				/* The master's next COMPLETE push is the one we answer; the
				 * transfer in flight across the edge is not it. */
				saw_first = 0; mids = 0;
				fprintf(stderr, "fake_box: PHY LINK-UP after a link-down — "
				        "this box was DEAF to %ld complete scene transfer(s) "
				        "and is now a box that has just booted\n",
				        deaf_transfers);
			}
			if (c >= 0)
				carrier_was = c;
		}

		now = mono_ns();
		memcpy(to.sll_addr, master, 6);
		if (commit_at && now >= commit_at && have_master) {
			size_t len = build(tx, master, COMMIT_REPORT);
			if (sendto(fd, tx, len, 0, (struct sockaddr *)&to, sizeof to) > 0)
				sent++;
			committed = 1; commit_at = 0;
			next_stream = now;
			fprintf(stderr, "fake_box: commit report sent after a COMPLETE "
			        "transfer (%ld complete, %ld ignored for want of a FIRST)\n",
			        transfers, ignored);
		}
		if (join_at && now >= join_at) {
			for (int i = 0; i < 3; i++) {
				size_t len = build(tx, master, JOIN_BURST[i]);
				if (sendto(fd, tx, len, 0, (struct sockaddr *)&to, sizeof to) > 0)
					sent++;
			}
			joined = 1; join_at = 0;
			fprintf(stderr, "fake_box: 04 03 burst sent, 1.489 s after the "
			        "master's ENROLL group map\n");
		}
		/* Once committed the box streams — a linked box is never silent again. */
		if (committed && now >= next_stream) {
			size_t len = reac_ctrl_build_upstream_filler(tx, master, BOX_MAC,
			                                             (uint16_t)sent,
			                                             BOX_CHANNELS, NULL,
			                                             REAC_SAMPLES_PER_PKT);
			if (len && sendto(fd, tx, len, 0, (struct sockaddr *)&to,
			                  sizeof to) > 0)
				sent++;
			next_stream = now + 1000000ull;     /* ~1 kHz; enough to stay linked */
		}
	}
	fprintf(stderr, "fake_box: %ld complete transfers seen, %ld ignored, "
	        "%ld frames sent, committed=%d joined=%d dropped=%d deaf_transfers=%ld "
	        "phy_edge=%d\n",
	        transfers, ignored, sent, committed, joined, dropped, deaf_transfers,
	        phy_edge);
	close(fd);
	/* In dropped mode committing is not enough: it has to have taken an EDGE to get
	 * there, and the master has to have spent whole pushes before it. A run that
	 * commits without either is a fake box that was never dropped. */
	if (dropped)
		return (committed && phy_edge && deaf_transfers > 0) ? 0 : 3;
	return committed ? 0 : 3;
}
