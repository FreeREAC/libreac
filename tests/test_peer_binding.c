// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* THE MASTER ANSWERS TO ITS BOX AND TO NOTHING ELSE ON THE WIRE.
 *
 * Driven through the real pacer ingest (reac_pacer_rx_ingest) with frames that pass every
 * other check: a Roland OUI, a valid checksum, unicast to us. Before the peer binding, one
 * frame from a MAC that was not the box rewrote the established box's width and base
 * (config-announce), dropped it (BYE), or took the session over (JOIN). Each of those is
 * now refused while a box is bound, and each arm carries its control: the SAME frame from
 * the box itself still does what it always did, and a box swap (the old box silent, the
 * link budget drained) still lets the next box in.
 *
 * Sabotage: make reac_master_peer_accepts return 1 and arms A, B, C and D go red. */
#define _GNU_SOURCE
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <reac/reac.h>
#include <reac/reac_ctrl.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_master.h>
#include <reac/transport/reac_pacer.h>

static int fails;
#define CHK(c) do { \
	if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
} while (0)

static const uint8_t US[6]   = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t BOX[6]  = { 0x00, 0x40, 0xab, 0x01, 0x01, 0x01 };
static const uint8_t EVIL[6] = { 0x00, 0x40, 0xab, 0x06, 0x06, 0x06 };
static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* The box declares 16 in / 8 out at strap 2; the forged one 40 in at base 0. */
static const uint8_t CFG16[32] = { 0x01, 0x03, 0x00, 0x10, 0x82, 0, 0, 0x02,
                                   2, 2, 2, 2, 1, 1, 3, 3, 3, 3, 3, 3 };
static const uint8_t CFG40[32] = { 0x01, 0x03, 0x00, 0x10, 0x84, 0, 0, 0x00,
                                   2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3 };
static const uint8_t BYE[32]   = { 0x01, 0x03, 0x00, 0x01, 0x00 };
static const uint8_t JOIN[32]  = { 0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe, 0x0f, 0xf0,
                                   0x41, 0x0a, 0, 0, 0x12, 0x12, 0x01, 0x00, 0x06, 0x00,
                                   0x01, 0x00, 0x78, 0xf7 };

static uint8_t f[REAC_FRAME_BYTES];

static size_t blockframe(const uint8_t dst[6], const uint8_t src[6], const uint8_t blk[32])
{
	memset(f, 0, sizeof f);
	memcpy(f, dst, 6);
	memcpy(f + 6, src, 6);
	f[12] = 0x88; f[13] = 0x19;
	f[16] = 0xcd; f[17] = 0xea;
	memcpy(f + 18, blk, 32);
	size_t n = 52 + 8 * 36;
	f[n - 2] = REAC_END_MARKER_0;
	f[n - 1] = REAC_END_MARKER_1;
	reac_ctrl_checksum_apply(f);
	return n;
}

static void slots(struct reac_pacer *p, int n)
{
	for (int i = 0; i < n; i++) {
		uint16_t c;
		int x;
		reac_master_next(&p->master, &c, &x);
	}
}

static void ingest(struct reac_pacer *p, const uint8_t dst[6], const uint8_t src[6],
                   const uint8_t blk[32])
{
	size_t n = blockframe(dst, src, blk);
	reac_pacer_rx_ingest(p, f, n);
}

/* The box declares itself and is granted; returns 1 once ESTABLISHED. */
static int establish(struct reac_pacer *p, const uint8_t box[6])
{
	slots(p, 20000);
	ingest(p, US, box, CFG16);
	for (int i = 0; i < 60000 && p->master.state != REAC_M_ESTABLISHED; i++) {
		slots(p, 1);
		if (i % 400 == 0)
			ingest(p, US, box, CFG16);
	}
	return p->master.state == REAC_M_ESTABLISHED;
}

static struct reac_pacer *fresh(void)
{
	struct reac_pacer *p = calloc(1, sizeof *p);
	if (!p)
		return NULL;
	memcpy(p->src, US, 6);
	struct reac_console_cfg idle = REAC_CONSOLE_CFG_IDLE;
	reac_master_init(&p->master, US, &idle, 4000);
	return p;
}

int main(void)
{
	struct reac_pacer *p = fresh();
	CHK(p != NULL);
	if (!p)
		return 1;
	CHK(establish(p, BOX));
	CHK(p->master.alloc.width == 16);

	/* A. another MAC's config-announce does not re-declare the box */
	ingest(p, US, EVIL, CFG40);
	CHK(p->master.state == REAC_M_ESTABLISHED);
	CHK(p->master.alloc.width == 16 && p->declared_in == 16);
	CHK(memcmp(p->master.box_mac, BOX, 6) == 0);

	/* B. another MAC's BYE does not drop the box */
	ingest(p, US, EVIL, BYE);
	CHK(p->master.state == REAC_M_ESTABLISHED);
	CHK(memcmp(p->master.box_mac, BOX, 6) == 0);

	/* C. another MAC's JOIN does not take the established session over */
	ingest(p, BCAST, EVIL, JOIN);
	CHK(p->master.state == REAC_M_ESTABLISHED);
	CHK(memcmp(p->master.box_mac, BOX, 6) == 0);

	/* D. another MAC's frames do not keep a silent box's link alive: the budget
	 * drains while only EVIL speaks, exactly as if nothing did */
	{
		int dropped = 0;
		for (int i = 0; i < 400000 && !dropped; i++) {
			slots(p, 1);
			if (i % 50 == 0) {
				uint8_t hb[32] = { 0x01, 0x03, 0x00, 0x01, 0x81 };
				ingest(p, US, EVIL, hb);
			}
			dropped = (p->master.state == REAC_M_PROBING);
		}
		CHK(dropped);
		CHK(p->master.drop_reason == REAC_M_DROP_PEER_GONE);
	}

	/* CONTROL 1. a box swap still works: the old box gone, the next JOIN courts the new */
	CHK(p->master.state == REAC_M_PROBING);
	CHK(establish(p, EVIL));
	CHK(memcmp(p->master.box_mac, EVIL, 6) == 0);
	free(p);

	/* CONTROL 2. the box's OWN frames still act: its BYE drops it */
	p = fresh();
	CHK(p && establish(p, BOX));
	if (p) {
		ingest(p, US, BOX, BYE);
		CHK(p->master.state == REAC_M_PROBING);
		CHK(p->master.drop_reason == REAC_M_DROP_BYE);
		free(p);
	}

	/* CONTROL 3. PROBING binds nothing; GRANTING still re-latches on another JOIN */
	{
		struct reac_master m;
		struct reac_console_cfg idle = REAC_CONSOLE_CFG_IDLE;
		reac_master_init(&m, US, &idle, 4000);
		CHK(reac_master_peer_accepts(&m, EVIL, REAC_M_RX_BOX_BYE));
		m.state = REAC_M_GRANTING;
		memcpy(m.box_mac, BOX, 6);
		CHK(reac_master_peer_accepts(&m, EVIL, REAC_M_RX_BOX_JOIN));
		CHK(!reac_master_peer_accepts(&m, EVIL, REAC_M_RX_BOX_CONFIG));
		CHK(!reac_master_peer_accepts(&m, EVIL, REAC_M_RX_BOX_UNICAST));
		CHK(!reac_master_peer_accepts(&m, EVIL, REAC_M_RX_BOX_BYE));
		CHK(reac_master_peer_accepts(&m, BOX, REAC_M_RX_BOX_BYE));
		m.state = REAC_M_ESTABLISHED;
		CHK(!reac_master_peer_accepts(&m, EVIL, REAC_M_RX_BOX_JOIN));
		CHK(!reac_master_peer_accepts(&m, EVIL, REAC_M_RX_BOX_HEARTBEAT));
	}

	if (fails) {
		fprintf(stderr, "%d peer-binding check(s) failed\n", fails);
		return 1;
	}
	printf("OK: test_peer_binding — once a box is bound, another MAC's config-announce, BYE, "
	       "JOIN and heartbeat neither re-declare, drop, take over nor keep alive the box; the "
	       "box's own BYE still drops it, a box swap still courts the next box, PROBING binds "
	       "nothing and GRANTING still re-latches on a JOIN\n");
	return 0;
}
