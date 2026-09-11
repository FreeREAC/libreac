/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The userspace backend's view of struct reac_handle: one POSIX descriptor.
 * Private to transport/src — never installed (include/reac/transport/reac_handle.h
 * carries only the incomplete type). */
#ifndef REAC_HANDLE_PRIV_H
#define REAC_HANDLE_PRIV_H

#include <reac/transport/reac_handle.h>

struct reac_handle {
	int fd;
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
