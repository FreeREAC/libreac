// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The one door every AF_PACKET socket in this tree goes through. The law, the two
// outages that bought it and why a transmit-only socket asks for protocol 0 are in
// include/reac/reac_packet_socket.h.

#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#include "reac/reac_packet_socket.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>

int reac_packet_socket_deaf(int extra_flags)
{
	/* PROTOCOL 0. This is the whole point: the kernel registers no receive hook
	 * until the bind, so the socket cannot hear a link it was never bound to. */
	return socket(AF_PACKET, SOCK_RAW | extra_flags, 0);
}

int reac_packet_socket_bind(int fd, int ifindex, uint16_t proto)
{
	struct sockaddr_ll sll;
	memset(&sll, 0, sizeof sll);
	sll.sll_family   = AF_PACKET;
	sll.sll_protocol = htons(proto);   /* the protocol arrives HERE, with the device */
	sll.sll_ifindex  = ifindex;
	return bind(fd, (struct sockaddr *)&sll, sizeof sll) < 0 ? -1 : 0;
}

int reac_packet_socket_bound(int ifindex, uint16_t proto, int extra_flags)
{
	int fd = reac_packet_socket_deaf(extra_flags);
	if (fd < 0)
		return -1;
	if (reac_packet_socket_bind(fd, ifindex, proto) != 0) {
		int saved = errno;
		close(fd);
		errno = saved;   /* close() must not decide what the caller reports */
		return -1;
	}
	return fd;
}

#else /* non-Linux: AF_PACKET does not exist, and saying so is the honest answer */

int reac_packet_socket_deaf(int extra_flags)
{
	(void)extra_flags;
	errno = EAFNOSUPPORT;
	return -1;
}

int reac_packet_socket_bind(int fd, int ifindex, uint16_t proto)
{
	(void)fd; (void)ifindex; (void)proto;
	errno = EAFNOSUPPORT;
	return -1;
}

int reac_packet_socket_bound(int ifindex, uint16_t proto, int extra_flags)
{
	(void)ifindex; (void)proto; (void)extra_flags;
	errno = EAFNOSUPPORT;
	return -1;
}

#endif
