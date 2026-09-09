// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The PURE half of reac_macaddr.h: composing an emitting role's address, and packing one
 * into a word. The impure half — reading SIOCGIFHWADDR off a named interface — is the
 * daemon's, and stays there with the sockets; nothing in this library opens one.
 *
 * These moved here with reac_link_state.c, which calls reac_mac48_unpack: a header that
 * declares a function whose definition stayed in the daemon builds a libreac.so with an
 * undefined symbol, and every consumer's link fails for a caller inside this library. */

#include <reac/reac_macaddr.h>

#include <string.h>
#include <net/if_arp.h>   /* ARPHRD_ETHER */

/* The fallback, used only when the NIC hwaddr can't be read: locally
 * administered (bit 0x02 in the first octet), so it is by construction not any
 * manufacturer's address and cannot collide with real gear on the wire. */
static const uint8_t fallback_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

int reac_mac_compose(int hw_family, const uint8_t hwaddr[6], uint8_t out[6])
{
	if (hw_family == ARPHRD_ETHER && hwaddr) {
		memcpy(out, hwaddr, 6);
		return 0;
	}
	memcpy(out, fallback_mac, 6);
	return -1;
}

int reac_mac_fallback(uint8_t out[6])
{
	memcpy(out, fallback_mac, 6);
	return -1;
}

int reac_mac_roland_standin(const uint8_t hwaddr[6], uint8_t out[6])
{
	/* Roland's OUI, and this NIC's own host part behind it (see the header). */
	out[0] = 0x00; out[1] = 0x40; out[2] = 0xab;
	out[3] = hwaddr[3]; out[4] = hwaddr[4]; out[5] = hwaddr[5];
	return 0;
}

uint64_t reac_mac48_pack(const uint8_t mac[6])
{
	uint64_t v = 0;
	if (!mac)
		return 0;
	for (int i = 0; i < 6; i++)
		v = (v << 8) | (uint64_t)mac[i];
	return v;
}

void reac_mac48_unpack(uint64_t packed, uint8_t out[6])
{
	if (!out)
		return;
	for (int i = 5; i >= 0; i--) {
		out[i] = (uint8_t)(packed & 0xffu);
		packed >>= 8;
	}
}
