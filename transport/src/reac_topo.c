/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * reac_topo — the tag classifier, the per-parent VLAN table, and the ETH_P_ALL tap that
 * feeds them; see reac_topo.h for the measurements this is built on.
 */
#include <reac/transport/reac_topo.h>
#include <reac/reac_packet_socket.h> /* the one door every AF_PACKET socket goes through */
#include "reac_handle_priv.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>

#define REAC_ETHERTYPE 0x8819
#define VLAN_CTAG      0x8100   /* 802.1Q */
#define VLAN_STAG      0x88a8   /* 802.1ad — an outer tag on a QinQ trunk */

const char *reac_topo_kind_name(enum reac_topo_kind k)
{
	switch (k) {
	case REAC_TOPO_NOT_REAC:     return "not-reac";
	case REAC_TOPO_UNTAGGED:     return "untagged";
	case REAC_TOPO_TAGGED:       return "tagged";
	case REAC_TOPO_TAGGED_OTHER: return "tagged-other";
	}
	return "?";
}

const char *reac_topo_verb_name(enum reac_topo_verb v)
{
	switch (v) {
	case REAC_TOPO_NONE:    return "none";
	case REAC_TOPO_ENSURE:  return "ensure";
	case REAC_TOPO_RELEASE: return "release";
	}
	return "?";
}

static uint16_t be16at(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

enum reac_topo_kind reac_topo_classify(const void *buf, size_t len, int tci_valid,
                                       uint16_t tci, uint16_t *vid_out)
{
	const uint8_t *b = buf;
	uint16_t vid = 0;

	if (vid_out)
		*vid_out = 0;
	if (!b || len < 14)
		return REAC_TOPO_NOT_REAC;

	/* The kernel's own answer first. VID 0 is a priority tag and names no VLAN, so it
	 * leaves `vid` at 0 and the frame goes on to read as untagged — `<parent>.0` is not
	 * a netdev anyone wants. */
	if (tci_valid)
		vid = (uint16_t)(tci & 0x0fff);

	/* Then whatever tags the driver left in the bytes. The FIRST of them names the VLAN
	 * only if the kernel supplied none; an accelerated outer tag outranks an inner one. */
	size_t off = 12;
	for (int depth = 0; depth < 2; depth++) {
		uint16_t et = be16at(b + off);
		if (et != VLAN_CTAG && et != VLAN_STAG)
			break;
		if (len < off + 4 + 2)
			return REAC_TOPO_NOT_REAC;
		if (vid == 0)
			vid = (uint16_t)(be16at(b + off + 2) & 0x0fff);
		off += 4;
	}
	if (vid_out)
		*vid_out = vid;
	/* A TAG NAMES A VLAN WHATEVER IT CARRIES (ruling 2026-09-22). On a cold rig no REAC
	 * frame is ever tagged — a stagebox is a slave and says nothing until a master
	 * speaks — so the switch's own STP/LLDP/broadcast traffic is the only thing that
	 * says the VLAN is there at all. It is a weaker fact than a REAC sighting and it is
	 * kept separate all the way up: this never reads as REAC_TOPO_TAGGED, so it never
	 * reaches the trunk verdict. */
	if (be16at(b + off) != REAC_ETHERTYPE)
		return vid ? REAC_TOPO_TAGGED_OTHER : REAC_TOPO_NOT_REAC;
	return vid ? REAC_TOPO_TAGGED : REAC_TOPO_UNTAGGED;
}

/* ---- the table -------------------------------------------------------------------- */

void reac_topo_init(struct reac_topo *t)
{
	memset(t, 0, sizeof *t);
}

static void emit(struct reac_topo *t, enum reac_topo_verb v, const char *parent,
                 uint16_t vid, int minted)
{
	int next = (t->ev_tail + 1) % REAC_TOPO_EVENTS;
	if (next == t->ev_head) {
		t->dropped_ev++;
		return;
	}
	t->ev[t->ev_tail].verb = v;
	snprintf(t->ev[t->ev_tail].parent, IFNAMSIZ, "%s", parent);
	t->ev[t->ev_tail].vid = vid;
	t->ev[t->ev_tail].minted = minted;
	t->ev_tail = next;
}

int reac_topo_next(struct reac_topo *t, struct reac_topo_event *out)
{
	if (t->ev_head == t->ev_tail)
		return 0;
	*out = t->ev[t->ev_head];
	t->ev_head = (t->ev_head + 1) % REAC_TOPO_EVENTS;
	return 1;
}

static struct reac_topo_parent *parent_of(struct reac_topo *t, const char *name)
{
	for (int i = 0; i < REAC_TOPO_MAX_PARENTS; i++)
		if (t->p[i].name[0] && strcmp(t->p[i].name, name) == 0)
			return &t->p[i];
	return NULL;
}

const struct reac_topo_parent *reac_topo_find(const struct reac_topo *t, const char *parent)
{
	for (int i = 0; i < REAC_TOPO_MAX_PARENTS; i++)
		if (t->p[i].name[0] && strcmp(t->p[i].name, parent) == 0)
			return &t->p[i];
	return NULL;
}

const struct reac_topo_vlan *reac_topo_vlan_find(const struct reac_topo *t, const char *parent,
                                                 uint16_t vid)
{
	const struct reac_topo_parent *p = reac_topo_find(t, parent);
	if (!p)
		return NULL;
	for (int i = 0; i < REAC_TOPO_MAX_VLANS; i++)
		if (p->v[i].state != REAC_TOPO_VLAN_FREE && p->v[i].vid == vid)
			return &p->v[i];
	return NULL;
}

int reac_topo_watch(struct reac_topo *t, const char *parent)
{
	if (parent_of(t, parent))
		return 0;
	for (int i = 0; i < REAC_TOPO_MAX_PARENTS; i++) {
		if (t->p[i].name[0])
			continue;
		memset(&t->p[i], 0, sizeof t->p[i]);
		snprintf(t->p[i].name, IFNAMSIZ, "%s", parent);
		return 0;
	}
	t->unbounded++;
	return -1;
}

/* Release every netdev we minted under this parent and forget it. A parent that lost its
 * carrier or its netdev carries nothing on any VLAN, so the sub-interfaces we created for
 * it have no reason to exist; adopted ones are the host's and are only forgotten. */
void reac_topo_unwatch(struct reac_topo *t, const char *parent, uint64_t now_ns)
{
	(void)now_ns;
	struct reac_topo_parent *p = parent_of(t, parent);
	if (!p)
		return;
	for (int i = 0; i < REAC_TOPO_MAX_VLANS; i++)
		if (p->v[i].state == REAC_TOPO_VLAN_SERVED)
			emit(t, REAC_TOPO_RELEASE, p->name, p->v[i].vid, p->v[i].minted);
	memset(p, 0, sizeof *p);
}

void reac_topo_saw(struct reac_topo *t, const char *parent, enum reac_topo_kind kind,
                   uint16_t vid, uint64_t now_ns)
{
	struct reac_topo_parent *p = parent_of(t, parent);
	if (!p || kind == REAC_TOPO_NOT_REAC)
		return;
	if (kind == REAC_TOPO_UNTAGGED) {
		p->untagged++;
		return;
	}
	if (vid == 0)
		return;
	/* ONLY TAGGED REAC MAKES A TRUNK, AND THE DESK IS WHY. `tagged` feeds
	 * reac_topo_is_trunk(), which decides whether the PARENT may be driven at all. On
	 * the rig enp131s0's S-4000 is heard UNTAGGED because VLAN 11 is that trunk port's
	 * native VLAN (measured 2026-09-10), so a trunk verdict drawn from one STP frame
	 * would stop that parent being driven and unserve a segment that works today.
	 * Hearing a VID and refusing to drive a parent are two questions about one frame,
	 * and only the second is about REAC. */
	if (kind == REAC_TOPO_TAGGED)
		p->tagged++;
	struct reac_topo_vlan *free_slot = NULL;
	for (int i = 0; i < REAC_TOPO_MAX_VLANS; i++) {
		struct reac_topo_vlan *v = &p->v[i];
		if (v->state == REAC_TOPO_VLAN_FREE) {
			if (!free_slot)
				free_slot = v;
			continue;
		}
		if (v->vid != vid)
			continue;
		v->frames++;
		v->last_ns = now_ns;
		/* A HEARD VID whose ensure failed retries once its window is spent — the
		 * caller sees a second ENSURE and tries again. */
		if (v->state == REAC_TOPO_VLAN_HEARD && v->retry_after_ns &&
		    now_ns >= v->retry_after_ns) {
			v->retry_after_ns = 0;
			emit(t, REAC_TOPO_ENSURE, p->name, vid, 0);
		}
		return;
	}
	if (!free_slot) {
		t->unbounded++;
		return;
	}
	memset(free_slot, 0, sizeof *free_slot);
	free_slot->vid = vid;
	free_slot->state = REAC_TOPO_VLAN_HEARD;
	free_slot->frames = 1;
	free_slot->last_ns = now_ns;
	emit(t, REAC_TOPO_ENSURE, p->name, vid, 0);
}

static struct reac_topo_vlan *vlan_of(struct reac_topo *t, const char *parent, uint16_t vid)
{
	struct reac_topo_parent *p = parent_of(t, parent);
	if (!p)
		return NULL;
	for (int i = 0; i < REAC_TOPO_MAX_VLANS; i++)
		if (p->v[i].state != REAC_TOPO_VLAN_FREE && p->v[i].vid == vid)
			return &p->v[i];
	return NULL;
}

void reac_topo_ensured(struct reac_topo *t, const char *parent, uint16_t vid, int minted)
{
	struct reac_topo_vlan *v = vlan_of(t, parent, vid);
	if (!v)
		return;
	v->state = REAC_TOPO_VLAN_SERVED;
	v->minted = minted ? 1 : 0;
	v->retry_after_ns = 0;
}

void reac_topo_ensure_failed(struct reac_topo *t, const char *parent, uint16_t vid,
                             uint64_t now_ns)
{
	struct reac_topo_vlan *v = vlan_of(t, parent, vid);
	if (!v)
		return;
	v->state = REAC_TOPO_VLAN_HEARD;
	v->minted = 0;
	v->retry_after_ns = now_ns + REAC_TOPO_RETRY_NS;
}

void reac_topo_tick(struct reac_topo *t, uint64_t now_ns)
{
	for (int i = 0; i < REAC_TOPO_MAX_PARENTS; i++) {
		struct reac_topo_parent *p = &t->p[i];
		if (!p->name[0])
			continue;
		for (int j = 0; j < REAC_TOPO_MAX_VLANS; j++) {
			struct reac_topo_vlan *v = &p->v[j];
			if (v->state != REAC_TOPO_VLAN_SERVED)
				continue;
			if (now_ns < v->last_ns + REAC_TOPO_SILENCE_HOLD_NS)
				continue;
			emit(t, REAC_TOPO_RELEASE, p->name, v->vid, v->minted);
			memset(v, 0, sizeof *v);
		}
	}
}

void reac_topo_release_all(struct reac_topo *t)
{
	for (int i = 0; i < REAC_TOPO_MAX_PARENTS; i++) {
		struct reac_topo_parent *p = &t->p[i];
		if (!p->name[0])
			continue;
		for (int j = 0; j < REAC_TOPO_MAX_VLANS; j++) {
			struct reac_topo_vlan *v = &p->v[j];
			if (v->state != REAC_TOPO_VLAN_SERVED)
				continue;
			emit(t, REAC_TOPO_RELEASE, p->name, v->vid, v->minted);
			memset(v, 0, sizeof *v);
		}
	}
}

int reac_topo_is_trunk(const struct reac_topo *t, const char *parent)
{
	const struct reac_topo_parent *p = reac_topo_find(t, parent);
	return p && p->tagged > 0;
}

int reac_topo_untagged_on_trunk(struct reac_topo *t, const char *parent)
{
	struct reac_topo_parent *p = parent_of(t, parent);
	if (!p || p->tagged == 0 || p->untagged == 0 || p->said_untagged)
		return 0;
	p->said_untagged = 1;
	return 1;
}

int reac_topo_count(const struct reac_topo *t, const char *parent, enum reac_topo_vstate st)
{
	const struct reac_topo_parent *p = reac_topo_find(t, parent);
	int n = 0;
	if (!p || st == REAC_TOPO_VLAN_FREE)
		return 0;
	for (int i = 0; i < REAC_TOPO_MAX_VLANS; i++)
		if (p->v[i].state == st)
			n++;
	return n;
}

int reac_topo_heard_vids(const struct reac_topo *t, const char *parent, uint16_t *out, int max)
{
	const struct reac_topo_parent *p = reac_topo_find(t, parent);
	int n = 0;
	if (!p)
		return 0;
	for (int i = 0; i < REAC_TOPO_MAX_VLANS; i++) {
		if (p->v[i].state == REAC_TOPO_VLAN_FREE)
			continue;
		if (out && n < max)
			out[n] = p->v[i].vid;
		n++;
	}
	return (out && n > max) ? max : n;
}

int reac_topo_is_stacked(const char *root, const char *ifname)
{
	if (!ifname || !ifname[0])
		return 0;
	char path[512];
	snprintf(path, sizeof path, "%s/%s", root ? root : "/sys/class/net", ifname);
	DIR *d = opendir(path);
	if (!d)
		return 0;
	int stacked = 0;
	const struct dirent *e;
	while (!stacked && (e = readdir(d)) != NULL)
		if (strncmp(e->d_name, "lower_", 6) == 0)
			stacked = 1;
	closedir(d);
	return stacked;
}

/* ---- the socket shell ------------------------------------------------------------- */

/* REAC, OR ANYTHING THAT CARRIES A TAG, AND NOTHING ELSE. An ETH_P_ALL socket on a trunk
 * without a filter copies every frame on the link to userspace, so this is the load guard
 * as much as the classifier's front door, and each arm is here for a different fact.
 *
 * 0x8819 AT 12 IS REAC WITH THE TAG ACCELERATED AWAY, OR NO TAG AT ALL — the kernel hands
 * us the frame with the 802.1Q header already out of the bytes (measured; see the header),
 * and `tp_vlan_tci` is what tells the two apart. Passed WHOLE: this is the audio protocol's
 * own frame and the arm that has always been here.
 *
 * 0x8100/0x88a8 AT 12 IS A TAG THE DRIVER LEFT IN THE BYTES. Whether a driver strips a tag
 * is a driver's business and a classifier that assumed one of them would go deaf on the
 * other. Passed whole too, and the classifier walks two levels for the QinQ case.
 *
 * THE ANCILLARY ARM IS THE 2026-09-22 RULING, AND IT IS THE WHOLE OF THE COLD-VLAN FIX.
 * `SKF_AD_VLAN_TAG_PRESENT` is the kernel's own "this frame arrived tagged" bit — the same
 * one tcpdump's `vlan` primitive reads — and it is true for frames of EVERY ethertype. A
 * trunk port carries each VLAN's STP, LLDP, ARP and broadcast traffic tagged whatever the
 * boxes are doing, and on a cold rig that is the only evidence the VLAN exists at all
 * (nothing REAC is ever tagged there: a stagebox is a slave and says nothing until a master
 * speaks, and the master needs the netdev first). Without this arm the tap is deaf to every
 * such frame and reac-pw's 2026-09-16 spec §1 can never fire on a cold trunk.
 *
 * AND IT IS THE ARM THAT DOES THE WORK ON EVERY WIRE WE CAN MEASURE. Sabotage-measured
 * 2026-09-22 on veth in a netns (tests/test_topo_hears_vlans.c) and through the daemon
 * (reac-pw's hearing-finds-a-segment.sh): drop this arm and the cold VLAN is not heard at
 * all; drop the two in-buffer arms instead and every test stays green. The kernel
 * accelerates the tag on veth, and it does on the rig's NIC too (`rx-vlan-offload: on`,
 * measured 2026-09-10) — so the in-buffer arms are exercised by NO wire test anywhere,
 * only by the classifier's own unit arms, which hand it bytes directly. They stay because
 * whether a driver strips a tag is a driver's business and the one that does not is the
 * one nobody will be watching for.
 *
 * IT IS THE ONLY ARM THAT TRUNCATES. Everything it admits is a frame we want for ONE fact —
 * which VID it came from, which is metadata — so 64 bytes is all that is ever read of it
 * (the classifier reads at most 22, reac-pw's reader also wants the source MAC at 6). A
 * shared trunk's whole tagged load would otherwise be copied out in full for that one fact.
 * An UNTAGGED non-REAC frame — the bulk of a native VLAN's traffic — is still dropped in
 * the kernel and never costs a copy. */
#define REAC_TOPO_TAG_SNAP 64

static struct sock_filter reac_topo_bpf[] = {
	{ BPF_LD  | BPF_H   | BPF_ABS, 0, 0, 12 },              /* A = ethertype at 12     */
	{ BPF_JMP | BPF_JEQ | BPF_K,   6, 0, REAC_ETHERTYPE },  /* REAC              -> whole */
	{ BPF_JMP | BPF_JEQ | BPF_K,   5, 0, VLAN_CTAG },       /* tag in the bytes  -> whole */
	{ BPF_JMP | BPF_JEQ | BPF_K,   4, 0, VLAN_STAG },       /* QinQ outer        -> whole */
	{ BPF_LD  | BPF_B   | BPF_ABS, 0, 0,                    /* A = "arrived tagged?"   */
	  (unsigned)(SKF_AD_OFF + SKF_AD_VLAN_TAG_PRESENT) },
	{ BPF_JMP | BPF_JEQ | BPF_K,   1, 0, 0 },               /* no tag            -> drop  */
	{ BPF_RET | BPF_K,             0, 0, REAC_TOPO_TAG_SNAP },
	{ BPF_RET | BPF_K,             0, 0, 0 },               /* drop                    */
	{ BPF_RET | BPF_K,             0, 0, 0x40000 },         /* pass the whole frame    */
};

int reac_topo_tap_open(struct reac_topo_tap *t, const char *parent)
{
	t->handle = NULL;
	/* PROTOCOL 0 — THE SOCKET IS DEAF UNTIL IT IS BOUND (#18). A packet socket created
	 * with a NON-ZERO protocol registers its receive hook on EVERY interface inside
	 * socket() itself, so everything below — the filter, PACKET_AUXDATA, the ifindex
	 * lookup — happens while the queue fills from every link on the host. Measured
	 * 2026-09-17 on a veth pair: 88 632 foreign frames over 400 opens, ~220 per open.
	 * On the rig (2026-09-14) it was one frame per VLAN per start, and each one was
	 * treated as evidence that this parent carried a tagged trunk: a cold-cable NIC was
	 * refused a master for ever, from the daemon's own masters on ANOTHER parent.
	 *
	 * With protocol 0 the kernel registers no hook at all (net/packet/af_packet.c:
	 * packet_create hooks only when proto != 0), and the bind below installs it with the
	 * interface AND the protocol together — the one atomic step packet(7) offers. The
	 * filter is still attached first, so the hook is never live without it; that is why
	 * this site takes the two halves of the door separately instead of
	 * reac_packet_socket_bound().
	 *
	 * ETH_P_ALL MOVES TO THE BIND, IT DOES NOT GO AWAY. A socket bound to 0x8819 reads
	 * vlan_tci = none for a tagged frame and would report every trunk as an access port
	 * (reac_topo.h's measurement); ETH_P_ALL on the bound device is what keeps the tag
	 * and the outgoing frames visible. */
	int fd = reac_packet_socket_deaf(SOCK_NONBLOCK);
	if (fd < 0)
		return -1;

	struct sock_fprog prog = {
		.len = (unsigned short)(sizeof reac_topo_bpf / sizeof reac_topo_bpf[0]),
		.filter = reac_topo_bpf,
	};
	int saved;
	if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof prog) != 0)
		goto fail;

	/* THE WHOLE POINT OF THIS SOCKET. Without auxdata the tag is invisible and the
	 * detector reports every trunk as an access port. */
	int on = 1;
	if (setsockopt(fd, SOL_PACKET, PACKET_AUXDATA, &on, sizeof on) != 0)
		goto fail;

	unsigned idx = if_nametoindex(parent);
	if (idx == 0)
		goto fail;
	if (reac_packet_socket_bind(fd, (int)idx, ETH_P_ALL) != 0)
		goto fail;
	t->handle = reac_handle_adopt(fd);
	if (!t->handle)
		goto fail;
	return 0;
fail:
	saved = errno;
	close(fd);
	errno = saved;
	return -1;
}

int reac_topo_tap_fd(const struct reac_topo_tap *t)
{
	return reac_handle_fd(t->handle);
}

int reac_topo_tap_next(struct reac_topo_tap *t, enum reac_topo_kind *kind, uint16_t *vid)
{
	uint8_t frame[2048];
	uint8_t control[CMSG_SPACE(sizeof(struct tpacket_auxdata))];
	struct iovec iov = { .iov_base = frame, .iov_len = sizeof frame };
	struct msghdr msg;
	memset(&msg, 0, sizeof msg);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof control;

	ssize_t n = recvmsg(reac_handle_fd(t->handle), &msg, 0);
	if (n < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;

	int tci_valid = 0;
	uint16_t tci = 0;
	for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
		if (cm->cmsg_level != SOL_PACKET || cm->cmsg_type != PACKET_AUXDATA)
			continue;
		struct tpacket_auxdata aux;
		memcpy(&aux, CMSG_DATA(cm), sizeof aux);
		/* TP_STATUS_VLAN_VALID is what separates "vid 0" from "no tag": the kernel
		 * zeroes tp_vlan_tci for an untagged frame and for a priority-tagged one
		 * alike, and only this bit says which. */
		if (aux.tp_status & TP_STATUS_VLAN_VALID) {
			tci_valid = 1;
			tci = aux.tp_vlan_tci;
		}
	}
	enum reac_topo_kind k = reac_topo_classify(frame, (size_t)n, tci_valid, tci, vid);
	if (kind)
		*kind = k;
	return 1;
}

void reac_topo_tap_close(struct reac_topo_tap *t)
{
	reac_handle_close(&t->handle);
}
