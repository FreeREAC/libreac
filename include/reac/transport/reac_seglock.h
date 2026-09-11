// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_seglock — one master per segment, enforced across PROCESSES.
 *
 * "Never two masters on a segment" has been a rule for months and has been
 * broken twice: once costing an evening, and once when reac-pw-master.service
 * self-activated a second master at 96 kHz onto a live segment and corrupted a
 * measurement in progress. A rule you have to remember is not a mechanism.
 *
 * The mechanism is an abstract-namespace AF_UNIX socket (trunk-VLAN spec §6),
 * bound to
 *
 *     \0reac-pw/segment/<netns-inode>/<ifindex>
 *
 * Whoever binds first holds the segment; the loser gets EADDRINUSE and refuses
 * that segment BY NAME. There is no lock file to go stale and no cleanup path to
 * get wrong: the kernel drops the binding when the holder exits, however it exits.
 *
 * THE NAME IS SHARED WITH THE KERNEL MODULE, which binds the identical string
 * from a kernel socket in the same netns (reac-kmod docs/coexistence.md §4). It
 * is spelled "reac-pw/" even though the module is one of the parties because the
 * name identifies THE SEGMENT, not the binary holding it — a neutral rename would
 * be a second namespace wearing the first one's clothes, and every deployed
 * daemon would stop seeing the module. Do not respell it.
 *
 * BINDING IS NOT MASTERING. RX is a copy: observing, counting or capturing a
 * segment takes no lock and must stay safe beside a live master. Only asserting
 * mastery claims it, and only immediately before the first frame goes out.
 *
 * NOTHING AUTO-ESCALATES. There is no takeover, no retry loop and no priority
 * order — an automatic winner between two correct implementations is a coin toss
 * the operator cannot predict, and auto-takeover is exactly how a second master
 * landed on a segment before. A refusal is final for that segment and leaves
 * every other segment running.
 *
 * The holder is visible in /proc/net/unix as an @-prefixed name, so "who has this
 * segment" is answerable without either party's cooperation.
 */
#ifndef REAC_SEGLOCK_H
#define REAC_SEGLOCK_H

#include <reac/transport/reac_handle.h>

struct reac_seglock {
	struct reac_handle *handle;  /* the claim; NULL when we hold nothing (reac_handle.h) */
	char name[128];  /* the abstract name, for messages (leading NUL shown as @) */
};

/* Claim `ifname` for DRIVING. 0 on success, -1 if another process (or the kernel
 * module) already holds it, -2 if the segment could not be identified at all.
 * On -1 the caller must refuse this segment by name and carry on with others. */
/* A lock that holds nothing; what a zeroed struct also means. */
void reac_seglock_init(struct reac_seglock *l);

/* 1 while we hold the segment, 0 otherwise — the only question a caller may ask of
 * the claim itself. */
int  reac_seglock_held(const struct reac_seglock *l);

int  reac_seglock_claim(struct reac_seglock *l, const char *ifname);

/* Release. Safe on an unheld lock; exiting releases it anyway. */
void reac_seglock_release(struct reac_seglock *l);

#endif /* REAC_SEGLOCK_H */
