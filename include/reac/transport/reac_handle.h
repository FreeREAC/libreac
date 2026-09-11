/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The transport's OS handle, backend-private (spec 2026-09-11-reac-transport-library §3).
 *
 * Every transport object that reaches the segment or the host (reac_tx, reac_pacer,
 * reac_slave, reac_seglock, reac_ifscan) holds ONE of these and nothing else that names
 * how it reaches them. Its layout is complete only inside transport/src: the userspace
 * backend keeps an AF_PACKET / AF_NETLINK / AF_UNIX descriptor in it; a kernel-module
 * backend would keep whatever it needs, and no public struct changes shape for that.
 * NULL means "not open". The library allocates it in the object's own open/claim call
 * (control plane, never the RT path) and frees it in close/release; a caller never
 * creates, reads or frees one. */
#ifndef REAC_HANDLE_H
#define REAC_HANDLE_H

struct reac_handle;

#endif
