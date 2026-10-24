// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libFuzzer harness: DISCOVERY (src/reac_disco.c) and the HUNT built on it
 * (src/reac_hunt.c) — who is on a segment, in which role, and what the table a
 * consumer reads says about them.
 *
 * Input:  [0..5] our MAC   [6] bit 0: pin the hunt's role, bit 1: the pinned role,
 *                          bit 2: the wire was proven masterless (the knock's licence)
 *         then operations, at most 512, each [op][u16 LE dt in ms] and, for a frame
 *         operation, a u16 LE length and the frame's bytes:
 *           op & 3 == 0   classify unlocked -> announce gate -> table
 *           op & 3 == 1   classify against the segment's peer lock -> gate -> table
 *           op & 3 == 2   offer the frame to the hunt
 *           op & 3 == 3   age the table, serialise it into a buffer of
 *                         k = op >> 2 & 0x1f: (k - 1) * 41 + 2 bytes, or
 *                         REAC_DISCO_JSON_MAX for k = 0; step the hunt
 *         op & 0x04 (frame ops): bypass the gate and observe directly
 *         op & 0x08 (frame ops): the sighting is our own peer (`owned`)
 *         op & 0x10 (frame ops): the sighting carries the box family (op >> 5 & 3)
 *                    an established master learnt from the peer's identity page
 *         op & 0x80: the clock steps BACK by dt instead of forward — every age
 *                    computation here claims to survive a non-monotonic clock.
 *
 * The clock is the input's: a time the harness keeps, never a read of the real one.
 *
 * Properties, each an abort():
 *   - a sighting is the frame's own source, never our MAC, its geometry is the frame
 *     length's (for the hunt's settled verdict, at least that), and outside FILLER its
 *     block checksum closes;
 *   - the gate, the table and the hunt's table never hold more than REAC_DISCO_MAX
 *     peers, and never the same MAC twice;
 *   - A BOX DOES NOT UN-PROVE ITSELF: an entry the table holds as a box is never
 *     turned into a master by a later sighting;
 *   - the table's seq never runs backwards and moves on every reported change;
 *   - a full table always serialises into REAC_DISCO_JSON_MAX, and any successful
 *     serialisation is one NUL-terminated JSON array of the length returned.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_disco.h>
#include <reac/reac_hunt.h>

#include "fuzz.h"

#define MAX_OPS 512

static void check_table(const struct reac_disco_table *t)
{
	if (t->n < 0 || t->n > REAC_DISCO_MAX)
		abort();
	for (int i = 0; i < t->n; i++) {
		if ((int)t->e[i].role < REAC_DISCO_ROLE_UNKNOWN ||
		    t->e[i].role > REAC_DISCO_ROLE_MASTER)
			abort();
		for (int j = i + 1; j < t->n; j++)
			if (memcmp(t->e[i].mac, t->e[j].mac, 6) == 0)
				abort();
	}
}

static void check_gate(const struct reac_disco_gate *g)
{
	if (g->n < 0 || g->n > REAC_DISCO_MAX)
		abort();
	for (int i = 0; i < g->n; i++)
		for (int j = i + 1; j < g->n; j++)
			if (memcmp(g->e[i].mac, g->e[j].mac, 6) == 0)
				abort();
}

static int role_of(const struct reac_disco_table *t, const uint8_t mac[6])
{
	for (int i = 0; i < t->n; i++)
		if (memcmp(t->e[i].mac, mac, 6) == 0)
			return t->e[i].role;
	return -1;
}

/* `settled`: the hunt answers with the table's verdict for the peer, whose width is
 * the widest geometry the peer has shown, so it is at least this frame's. */
static void check_sighting(const struct reac_disco_sighting *s, const uint8_t *frame,
                           size_t len, const uint8_t our[6], int settled)
{
	if (len < 50 || memcmp(s->mac, frame + 6, 6) != 0 || memcmp(s->mac, our, 6) == 0)
		abort();
	if (settled ? s->channels < reac_frame_channels(len)
	            : s->channels != reac_frame_channels(len))
		abort();
	if ((int)s->role < REAC_DISCO_ROLE_UNKNOWN || s->role > REAC_DISCO_ROLE_MASTER)
		abort();
	int filler = frame[16] == 0x00 && frame[17] == 0x00;
	if (!filler && reac_ctrl_checksum_verify(frame) != 0)
		abort();
	if (s->has_decl && (s->decl_in > REAC_MAX_CHANNELS || s->decl_out > REAC_MAX_CHANNELS))
		abort();
}

static void observe(struct reac_disco_table *t, const struct reac_disco_sighting *s,
                    int owned, uint64_t now)
{
	int before = role_of(t, s->mac);
	uint32_t seq = t->seq;
	int changed = reac_disco_table_observe(t, s, owned, now);
	if (changed != 0 && changed != 1)
		abort();
	if (changed ? t->seq != seq + 1 : (t->seq != seq && t->seq != seq + 1))
		abort();   /* a refusal for room may bump seq once, to say it overflowed */
	if (before == REAC_DISCO_ROLE_BOX && role_of(t, s->mac) == REAC_DISCO_ROLE_MASTER)
		abort();
	check_table(t);
}

static void serialise(const struct reac_disco_table *t, uint64_t now, size_t cap)
{
	char *buf = malloc(cap ? cap : 1);
	if (!buf)
		abort();
	int r = reac_disco_table_json(t, now, buf, cap);
	if (r == -1) {
		/* "sized so a full table always serializes rather than being suppressed" */
		if (cap >= REAC_DISCO_JSON_MAX)
			abort();
	} else {
		if (r < 2 || (size_t)r >= cap || buf[r] != '\0' || strlen(buf) != (size_t)r ||
		    buf[0] != '[' || buf[r - 1] != ']')
			abort();
	}
	free(buf);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size >= FUZZ_INPUT_MAX)
		return 0;
	struct fuzz_in in = fuzz_in(data, size);
	uint8_t our[6];
	for (int i = 0; i < 6; i++)
		our[i] = fuzz_u8(&in);
	uint8_t setup = fuzz_u8(&in);

	uint64_t now = 1000000000ULL;   /* one second in: the clock starts wherever it likes */
	struct reac_disco_table t;
	struct reac_disco_gate g;
	struct reac_disco_peer_lock lock;
	struct reac_hunt *h = malloc(sizeof *h);
	if (!h)
		abort();
	reac_disco_table_init(&t);
	reac_disco_gate_init(&g);
	reac_disco_peer_lock_init(&lock);
	reac_hunt_init(h, our, now);
	if (setup & 1)
		reac_hunt_pin(h, (setup & 2) ? REAC_ROLE_MASTER : REAC_ROLE_SLAVE);
	if (setup & 4)
		reac_hunt_silence_proven(h);

	for (int i = 0; i < MAX_OPS && in.n; i++) {
		uint8_t op = fuzz_u8(&in);
		uint64_t dt = (uint64_t)fuzz_u16(&in) * 1000000ULL;
		if (op & 0x80)
			now = dt > now ? 0 : now - dt;
		else
			now += dt;

		if ((op & 3) == 3) {
			uint32_t seq = t.seq;
			int removed = reac_disco_table_age(&t, now);
			if (removed < 0 || (removed ? t.seq != seq + 1 : t.seq != seq))
				abort();
			check_table(&t);
			size_t k = op >> 2 & 0x1f;
			serialise(&t, now, k ? (k - 1) * 41 + 2 : REAC_DISCO_JSON_MAX);
			int ch = reac_hunt_step(h, now);
			if (ch != 0 && ch != 1)
				abort();
			if (!reac_hunt_verdict_name(h->verdict))
				abort();
			check_table(&h->table);
			(void)reac_hunt_role(h);
			(void)reac_hunt_heard_anything(h);
			continue;
		}

		const uint8_t *body;
		size_t len = fuzz_record(&in, &body);
		uint8_t *frame = fuzz_dup(body, len);
		struct reac_disco_sighting s;
		if ((op & 3) == 2) {
			int before = role_of(&h->table, len >= 12 ? frame + 6 : our);
			int r = reac_hunt_observe(h, frame, len, now, &s);
			if (r < -1 || r > 1)
				abort();
			if (r >= 0) {
				check_sighting(&s, frame, reac_frame_clean_len(len), our, 1);
				if (before == REAC_DISCO_ROLE_BOX && s.role == REAC_DISCO_ROLE_MASTER)
					abort();
			}
			check_table(&h->table);
		} else {
			int r = (op & 3) == 0 ? reac_disco_classify(frame, len, our, &s)
			                      : reac_disco_classify_on_segment(&lock, frame, len, our, &s);
			if (r != 0 && r != -1)
				abort();
			if (r == 0) {
				check_sighting(&s, frame, len, our, 0);
				int push = reac_disco_gate_should_push(&g, &s, now);
				if (push != 0 && push != 1)
					abort();
				check_gate(&g);
				if (op & 0x10)
					s.family = op >> 5 & 3;
				if (push || (op & 4))
					observe(&t, &s, (op & 8) != 0, now);
			}
		}
		free(frame);
	}
	free(h);
	return 0;
}
