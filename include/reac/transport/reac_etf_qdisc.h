// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_etf_qdisc — install, remove and read back the ETF qdisc, over rtnetlink.
 *
 * WHOSE SETTING THIS IS. The qdisc and the pacing backend are ONE setting, and the
 * app that owns a setting owns its configuration: the daemon installs the qdisc it
 * needs on the device it binds, and takes it away again when it exits. Nothing here
 * runs `tc`; the transport already speaks rtnetlink for the interface scan, the link
 * watch and the qdisc probe, and a subprocess helper is refused by ruling.
 *
 * WHY IT IS NOT OPTIONAL, AND WHY IT IS NOT LEFT BEHIND. With skip_sock_check the etf
 * qdisc drops every frame that carries NO launch time. So:
 *
 *   - a daemon on the ETF backend under a device with no etf qdisc stamps every frame
 *     and has every stamp ignored — the silent no-op that cost the prior art months;
 *   - a daemon on the THREAD backend under a LEFTOVER etf qdisc transmits NOTHING.
 *     Measured 2026-09-14 on the rig: 0 frames in a 60 s window and the box lost its
 *     master (reac-captures/pace-compare-2026-09-14/direct-link-table.txt, hazard 1).
 *
 * Both are the same rule from the two sides: whoever chooses the backend must also
 * make the device match it, on every start and on every exit.
 *
 * ETF SUPPORTS NO CHANGE OPERATION. `tc qdisc replace` on an existing etf root fails
 * with "Change operation not supported by specified qdisc" (same evening, hazard 2),
 * so reac_etf_qdisc_install REMOVES first and then adds. Two messages, never one.
 *
 * Every door here needs CAP_NET_ADMIN. The reac-pw RPM grants it by file capability;
 * a shell does not have it, and the refusal for that says so by errno rather than by
 * a message string. */
#ifndef REAC_ETF_QDISC_H
#define REAC_ETF_QDISC_H

#include <stdint.h>
#include <stddef.h>

/* The qdisc's own `delta`: how far ahead of a launch time the qdisc will accept a
 * packet, and how far ahead of it the kernel hands the packet to the driver. 300 us
 * is the value proven on reac_repacer's rig ports — 80 us was ear-validated on one
 * of them and read "slightly beepy" on another. It is one of the two terms of the
 * pacer's lead (reac_etf.h). */
#define REAC_ETF_QDISC_DELTA_NS  300000u

/* CLOCK_TAI as the qdisc reads it off the wire message. Spelled as the number the
 * golden buffer carries rather than as the <time.h> macro, because this is a
 * PROTOCOL field: a header whose CLOCK_TAI moved would silently change the bytes. */
#define REAC_ETF_QDISC_CLOCKID_TAI  11

/* TC_ETF_SKIP_SOCK_CHECK. Without it sch_etf drops every packet from a socket that
 * did not set SO_TXTIME, and the pacer's socket is not the only thing that transmits
 * on a segment. With it, an unstamped packet is not refused ON THE SOCKET CHECK — it
 * can still be dropped once the queue is non-empty, because a tstamp of 0 reads as
 * already expired. That is the hazard the two rules above exist for. */
#define REAC_ETF_QDISC_FLAG_SKIP_SOCK_CHECK  0x4u

/* ---- what is on the device ------------------------------------------------ *
 *
 * UNREADABLE IS NOT ABSENT. A dump that could not be made says so with its own value:
 * reporting "no etf" from a netlink socket that never opened is the broken-search
 * failure, and it would refuse a correctly configured rig. */
enum reac_etf_qdisc_state {
	REAC_ETF_QDISC_UNREADABLE = -1,  /* the dump failed — NOT the same as absent */
	REAC_ETF_QDISC_NONE       =  0,  /* the device has qdiscs, none of them etf */
	REAC_ETF_QDISC_PRESENT    =  1,  /* etf is attached: launch times are honoured */
};

/* Read `ifindex`'s qdisc table. `root_kind` (may be NULL) receives the name of the
 * ROOT qdisc found, so a caller can say what IS there instead of only what is not.
 * Accepts an etf qdisc ANYWHERE on the device: on a multiqueue NIC etf is attached
 * per TX queue under an `mq` root. */
enum reac_etf_qdisc_state reac_etf_qdisc_state(int ifindex, char *root_kind, size_t cap);

/* ---- the two doors -------------------------------------------------------- *
 *
 * Both return 0, or -errno — NEVER a strerror match. The errno IS the diagnosis and
 * each one has a different fix (reac_etf_qdisc_fix):
 *
 *   -EPERM        this process has no CAP_NET_ADMIN
 *   -ENOENT       the kernel has no sch_etf (add), or there was no root qdisc to
 *                 delete (remove) — both are what the kernel answers, measured
 *   -EOPNOTSUPP   the device refused an etf qdisc (an offload it cannot do)
 *   -EINVAL       the parameters were refused (delta, clockid) */

/* del-then-add an `etf clockid CLOCK_TAI delta <delta_ns> skip_sock_check` as the
 * ROOT qdisc of `ifindex`. `delta_ns` of 0 means REAC_ETF_QDISC_DELTA_NS.
 *
 * The remove half is best-effort: -ENOENT there means there was nothing to remove,
 * which is the ordinary case, and the add is what decides the return. */
int reac_etf_qdisc_install(int ifindex, uint32_t delta_ns);

/* Delete `ifindex`'s ROOT qdisc, putting the device back to its default.
 *
 * NEVER CALL THIS BLIND. A delete is verified before it is issued, not after: the
 * caller checks reac_etf_qdisc_state first and removes only an etf it found, so a
 * device carrying somebody else's fq_codel is never stripped by us. */
int reac_etf_qdisc_remove(int ifindex);

/* The operator's fix for one of the errnos above, as a sentence. Never NULL. */
const char *reac_etf_qdisc_fix(int err);

/* ---- the messages --------------------------------------------------------- *
 *
 * Built into a caller's buffer so they can be asserted BYTE FOR BYTE against what
 * iproute2 puts on the same socket, which is what tests/test_reac_etf.c does with a
 * strace capture of
 *
 *   tc qdisc add dev veth0 root etf clockid CLOCK_TAI delta 300000 skip_sock_check
 *
 * taken inside `unshare -rn`. A builder proven only against itself proves nothing.
 *
 * Return the byte length written, or 0 when `cap` is too small. */
#define REAC_ETF_QDISC_ADD_LEN  64u
#define REAC_ETF_QDISC_DEL_LEN  36u

size_t reac_etf_qdisc_add_msg(void *buf, size_t cap, int ifindex,
                              uint32_t delta_ns, uint32_t seq);
size_t reac_etf_qdisc_del_msg(void *buf, size_t cap, int ifindex, uint32_t seq);

#endif /* REAC_ETF_QDISC_H */
