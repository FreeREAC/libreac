// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The process-wide storage for struct reac_transport_tunables (reac_tunables.h),
 * shared by reac_rx.c (REAC_DEBUG) and reac_ifscan.c (REAC_IFACES_ALLOW_WIRELESS).
 * A copy of `allow_wireless`, not the caller's pointer: unlike reac-pw's own
 * REACPW_CLOCK_REF (a documented exception, forwarded into a long-lived node that
 * does not copy it), this string is only ever read synchronously inside
 * reac_ifscan's own netlink callback, so a fixed buffer here is enough and the
 * caller's storage need not outlive the call. */
#include "reac_transport_tunables_priv.h"

#include <stdio.h>

static struct reac_transport_tunables g_transport_tunables = REAC_TRANSPORT_TUNABLES_DEFAULT;
static char g_allow_wireless_buf[256];

void reac_transport_tunables_set(const struct reac_transport_tunables *t)
{
	if (!t) {
		g_transport_tunables = (struct reac_transport_tunables)REAC_TRANSPORT_TUNABLES_DEFAULT;
		g_allow_wireless_buf[0] = '\0';
		return;
	}
	g_transport_tunables.debug = t->debug ? 1 : 0;
	if (t->allow_wireless && t->allow_wireless[0]) {
		snprintf(g_allow_wireless_buf, sizeof g_allow_wireless_buf, "%s",
		         t->allow_wireless);
		g_transport_tunables.allow_wireless = g_allow_wireless_buf;
	} else {
		g_allow_wireless_buf[0] = '\0';
		g_transport_tunables.allow_wireless = NULL;
	}
}

const struct reac_transport_tunables *reac_transport_tunables_get(void)
{
	return &g_transport_tunables;
}
