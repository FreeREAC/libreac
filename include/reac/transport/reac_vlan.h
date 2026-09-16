/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * reac_vlan — the netdevs behind the topology detector: `<parent>.<vid>` adopted if it is
 * there, created over rtnetlink if it is not, and removed at the end only if WE made it.
 * (openmixer's 2026-08-23-reac-trunk-vlan-daemon.md §4c, §4d.)
 *
 * ONE STORE, ONE WRITER, APPLIED TO HOST NETWORK STATE: the daemon owns exactly the netdevs
 * it minted and nothing else. That dissolves the choice between "the daemon configures the
 * host" and "the host configures itself" — a fixed installation keeps its sub-interfaces in
 * its configuration management and we adopt them untouched; a bare machine on a trunk works
 * with nothing typed. Neither can clobber the other.
 *
 * THE MARK RIDES THE OBJECT (§4d). A netdev we create carries the interface alias
 * REAC_VLAN_ALIAS (`ip link set dev <parent>.<vid> alias reac-pw:minted`), written through
 * the same netlink socket that created it. It needs no second store — no state file to go
 * stale, no PID file to go wrong — and it lives exactly as long as the thing it describes.
 * It exists for the case a naive design gets silently wrong: an UNCLEAN exit leaves the
 * netdev behind, the next start finds it present, adopts it as the host's, and the leak
 * becomes permanent and invisible. A netdev carrying the alias is a leaked mint from a
 * previous run — adopted for use AND re-owned, so the next clean exit removes it. A netdev
 * without it is the host's, whatever its name looks like.
 *
 * WITHOUT CAP_NET_ADMIN NOTHING HERE WORKS AND NOTHING HERE IS FATAL. Every call returns
 * -1 with errno set (EPERM), the caller reports the VIDs it cannot serve and the `ip link`
 * commands that would fix it, and goes on listening. Adoption needs no capability, so a
 * host that pre-created its sub-interfaces is fully served by an unprivileged daemon.
 */
#ifndef REAC_VLAN_H
#define REAC_VLAN_H

#include <net/if.h>   /* IFNAMSIZ */
#include <stddef.h>
#include <stdint.h>

/* The mark, and it is compared EXACTLY. Anything else in a netdev's alias belongs to
 * whoever wrote it and means the netdev is not ours. */
#define REAC_VLAN_ALIAS "reac-pw:minted"

/* `<parent>.<vid>` — the name the kernel's own vlan tooling would use, so a pre-created
 * sub-interface and one of ours are the same object under the same name. Returns 0, or -1
 * when the result would not fit in IFNAMSIZ (a long NIC name plus a 4-digit VID): a
 * TRUNCATED interface name is a different interface, so it is refused rather than trimmed. */
int reac_vlan_name(const char *parent, uint16_t vid, char *out, size_t n);

/* Is `name` there, and is it OURS? Returns 1 present / 0 absent / -1 on a netlink error,
 * and sets `*ours` (may be NULL) to 1 when the netdev carries REAC_VLAN_ALIAS — a leaked
 * mint from a previous run, which is adopted for use and re-owned, never left to
 * accumulate. Needs no capability. */
int reac_vlan_query(const char *name, int *ours);

/* Create `<parent>.<vid>` as a VLAN device over `parent`, mark it with REAC_VLAN_ALIAS and
 * bring it up. Returns 0, or -1 with errno (EPERM without CAP_NET_ADMIN, EEXIST if the name
 * appeared underneath us, ENODEV if the parent went away). */
int reac_vlan_create(const char *parent, uint16_t vid);

/* Set `name`'s ADMIN FLAG, both directions, over the same rtnetlink socket the rest of this
 * file uses. One bit is written (`ifi_change = IFF_UP`); nothing else about the netdev is
 * touched. Returns 0, or -1 with errno (EPERM without CAP_NET_ADMIN, ENODEV if the name is
 * not there). Already in the requested state is 0.
 *
 * THE DOWN DIRECTION EXISTS FOR ONE CALLER AND ONE REASON: a stagebox leaves its dropped
 * state on PHY LINK-UP and on nothing else — "a data gap does NOT" (reac-firmware-re
 * REAC-PROTOCOL-FROM-SOURCE §10.2, and reac_carrier.h says the same). A master whose box
 * went quiet while the desk was away has no frame it can send that will bring it back, and
 * on 2026-09-16 that cost a live segment 73 minutes of correct probing into silence. The
 * edge the box needs is one the daemon can make on its OWN port. When and how seldom it may
 * is reac-pw's `reac_wake` and its spec — this function is the write, and it has no policy
 * in it at all.
 *
 * NOT for an interface this process does not drive. Taking a link down takes every VLAN
 * child down with it. */
int reac_link_admin(const char *name, int up);

/* Bring `name` up. An adopted netdev may be configured and DOWN, which carries no frames
 * and looks exactly like a box that is not talking. Returns 0/-1; already up is 0. */
int reac_vlan_up(const char *name);

/* Remove `name`. Only ever called for a netdev this daemon minted — the caller holds that
 * fact (reac_topo's `minted`), because by the time we are deleting, the alias may be all
 * that says so and it is not re-read here. Returns 0/-1. */
int reac_vlan_delete(const char *name);

#endif /* REAC_VLAN_H */
