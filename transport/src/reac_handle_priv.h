/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The userspace backend's view of struct reac_handle: one POSIX descriptor, plus
 * whatever the TX path's pacing backend needs to own the egress instant.
 * Private to transport/src — never installed (include/reac/transport/reac_handle.h
 * carries only the incomplete type).
 *
 * WHY THE ETF STATE LIVES HERE AND NOT IN struct reac_pacer. That struct's memory
 * layout is an INTERFACE — reac-pw embeds it and reads its members directly, and
 * tests/abi-layout.inc records the offset of every one of them. A field added there
 * moves everything behind it and segfaults an installed daemon built against the
 * previous header (the 2026-09-14 rollback, 99 restarts). The handle is the
 * transport spec's declared home for backend-private state precisely so a second
 * backend costs no public struct a single byte: it is an incomplete type, it
 * appears in no ABI table, and the seven objects that hold one hold only a pointer.
 *
 * The cost is honest and small: reac_ifscan, reac_linkmon and reac_seglock also
 * allocate a handle and carry these bytes unused. A control-plane malloc per object
 * per process. */
#ifndef REAC_HANDLE_PRIV_H
#define REAC_HANDLE_PRIV_H

#include <reac/transport/reac_handle.h>

#include "reac_etf.h"

struct reac_handle {
	int fd;

	/* ---- the TX pacing backend (reac_etf.h) ---------------------------------
	 * `etf_on` is 0 for the thread backend, which is the default and is byte- and
	 * timing-identical to the pacer as it was: nothing below is read at all. */
	int                  etf_on;
	uint32_t             etf_lead_ns;    /* how far ahead of launch we submit */
	struct reac_etf_grid etf_grid;       /* the exact launch grid */
	uint64_t             etf_refused;    /* frames the qdisc would not launch */
	uint8_t              etf_first_code; /* SO_EE_CODE of the first refusal seen */
	/* Why ETF is NOT running, when the DEFAULT asked for it and a precondition
	 * was missing: a static phrase from reac_etf_refusal_name, never allocated.
	 * NULL while ETF is running and while the operator asked for `thread`. The
	 * daemon publishes it (reac_pacer_backend_refusal), because a fallback the
	 * operator cannot see is a silent no-op with extra steps. */
	const char          *etf_refusal;
};

/* Wrap an open descriptor. NULL on ENOMEM — the caller still owns `fd` then and must
 * close it. Control-plane only. */
struct reac_handle *reac_handle_adopt(int fd);

/* close(2) the descriptor, free the handle, and NULL the caller's pointer. Safe on NULL. */
void reac_handle_close(struct reac_handle **h);

/* The descriptor for the RT paths and the reactor: one pointer read, no allocation.
 * -1 when not open, so every `fd < 0` guard keeps its meaning. */
static inline int reac_handle_fd(const struct reac_handle *h)
{
	return h ? h->fd : -1;
}

#endif
