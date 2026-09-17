// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Private accessor for reac_transport_tunables.c's process-wide storage. Two
 * consumers (reac_rx.c's REAC_DEBUG telemetry gate, reac_ifscan.c's wireless
 * allowlist) read the same struct that reac_transport_tunables_set() (public,
 * reac_tunables.h) writes; this header is how they reach it without either
 * duplicating the storage. */
#ifndef REAC_TRANSPORT_TUNABLES_PRIV_H
#define REAC_TRANSPORT_TUNABLES_PRIV_H

#include <reac/reac_tunables.h>

const struct reac_transport_tunables *reac_transport_tunables_get(void);

#endif /* REAC_TRANSPORT_TUNABLES_PRIV_H */
