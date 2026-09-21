/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * THE ONE DOOR EVERY AF_PACKET SOCKET IN THIS TREE GOES THROUGH, and the reason it is a
 * door at all.
 *
 * A packet socket created with a NON-ZERO protocol registers its receive hook on EVERY
 * interface on the host inside socket() itself (net/packet/af_packet.c: packet_create
 * hooks whenever proto != 0). Everything a caller does between socket() and bind() — an
 * ioctl for the ifindex, a BPF filter, PACKET_AUXDATA, a sockopt — happens while that
 * queue fills from every link in the machine, and what lands there reads out afterwards
 * as a frame from the interface the caller asked for. sockaddr_ll carries the ifindex, so
 * the truth is available; nothing that ever hit this bug was looking at it.
 *
 * IT HAS BEEN PAID FOR TWICE.
 *   #18, 2026-09-14 — reac_topo_tap_open(). 88 632 foreign frames over 400 opens on a
 *   veth pair; on the rig, one frame per VLAN per start, each read as evidence that THIS
 *   parent carried a tagged trunk, so a cold-cable NIC was refused a master for ever.
 *   #19, 2026-09-21 — reac_capture_open(), the same defect in the socket that never got
 *   #18's fix. reac-pw opened five sniffers in one second while an S-1608 mastered
 *   another NIC at 8000 fps; a point-to-point cable with a COLD S-0808 on it logged the
 *   S-1608's MAC as heard on that wire, and two latches downstream turned that one frame
 *   into nine minutes with eight inputs off the desk.
 *
 * SO THE PROTOCOL BELONGS TO bind(), NEVER TO socket(). bind() installs the interface and
 * the protocol together — the one atomic step packet(7) offers — and a socket created
 * with protocol 0 has no hook at all until then. This header is that rule made
 * mechanical: `tools/conformance-packet-socket.sh` refuses any other socket(AF_PACKET,
 * ...) in src/, transport/src/ and tools/, so the next socket cannot be written the way
 * these two were.
 *
 * A TRANSMIT-ONLY SOCKET ASKS FOR PROTOCOL 0 AND STAYS DEAF. sendto() takes the ifindex
 * from the sockaddr_ll the caller hands it and the ethertype from the frame's own bytes,
 * so a sender needs no protocol and no receive hook — reac_tx carried one for months and
 * queued every 0x8819 frame on the host into a socket nothing ever read.
 *
 * Internal to the two libraries. Declared publicly because libreac and libreac-transport
 * are separate build products and both open packet sockets; no consumer needs it.
 */
#ifndef REAC_PACKET_SOCKET_H
#define REAC_PACKET_SOCKET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A packet socket that hears NOTHING yet: SOCK_RAW | `extra_flags` (SOCK_NONBLOCK and
 * SOCK_CLOEXEC are the ones used here) with protocol 0. Use it when something must be set
 * up before the socket goes live — a BPF filter, PACKET_AUXDATA — and finish with
 * reac_packet_socket_bind(). Returns the fd, or -1 with errno set.
 *
 * On a platform without AF_PACKET it returns -1 with errno EAFNOSUPPORT. */
int reac_packet_socket_deaf(int extra_flags);

/* Make a deaf socket live on ONE interface: binds `fd` to `ifindex` with `proto` (a
 * HOST-ORDER ethertype — ETH_P_ALL, 0x8819; this converts it). proto 0 pins the socket to
 * the interface and leaves it deaf, which is what a transmit-only socket wants.
 * Returns 0, or -1 with errno set (the fd is left open and owned by the caller). */
int reac_packet_socket_bind(int fd, int ifindex, uint16_t proto);

/* The whole move, for the callers that need nothing in between: a deaf socket bound to
 * `ifindex` with `proto`. Returns the fd, or -1 with errno set — the fd is closed on
 * every failure path and errno survives the close. */
int reac_packet_socket_bound(int ifindex, uint16_t proto, int extra_flags);

#ifdef __cplusplus
}
#endif

#endif /* REAC_PACKET_SOCKET_H */
