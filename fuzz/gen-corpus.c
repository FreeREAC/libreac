// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Writes the seed corpus of every harness in fuzz/ (`make fuzz-corpus`).
 *
 * Every seed is built from what the repository already holds: the real control
 * blocks of tests/ctrl_fixtures.inc, the S-4000H-0832 frames of
 * tests/box_0832_fixtures.inc, the captured box returns of
 * tests/upstream_fixtures.inc, the scene body libreac ships, and frames the
 * library's own builders emit. Nothing is fetched and no private capture is read,
 * so the corpus regenerates byte for byte on any machine.
 *
 * Each seed is shaped the way its harness reads its input; the harness's header
 * comment is the grammar. Usage: fuzz-gen-corpus <corpus dir>. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_encode.h>
#include <reac/reac_identity.h>
#include <reac/reac_scene_body.h>

#include "ctrl_fixtures.inc"
#include "box_0832_fixtures.inc"
#include "upstream_fixtures.inc"

/* ---- a growable byte buffer, one seed at a time ---- */
struct seed {
	uint8_t *p;
	size_t n, cap;
};

static void put(struct seed *s, const void *p, size_t n)
{
	if (s->n + n > s->cap) {
		s->cap = (s->n + n) * 2 + 64;
		s->p = realloc(s->p, s->cap);
		if (!s->p) {
			perror("realloc");
			exit(1);
		}
	}
	memcpy(s->p + s->n, p, n);
	s->n += n;
}

static void put8(struct seed *s, unsigned v)
{
	uint8_t b = (uint8_t)v;
	put(s, &b, 1);
}

static void put16(struct seed *s, unsigned v)
{
	put8(s, v & 0xff);
	put8(s, v >> 8 & 0xff);
}

static void put32(struct seed *s, uint32_t v)
{
	put16(s, v & 0xffff);
	put16(s, v >> 16);
}

/* A length-prefixed record, as fuzz_record() reads it. */
static void put_rec(struct seed *s, const uint8_t *p, size_t n)
{
	put16(s, (unsigned)n);
	put(s, p, n);
}

static const char *g_root;

static void mkdir_p(const char *path)
{
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		perror(path);
		exit(1);
	}
}

static void emit(const char *harness, const char *name, struct seed *s)
{
	char dir[512], path[640];
	snprintf(dir, sizeof dir, "%s/%s", g_root, harness);
	mkdir_p(dir);
	snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "wb");
	if (!f || fwrite(s->p, 1, s->n, f) != s->n || fclose(f) != 0) {
		perror(path);
		exit(1);
	}
	s->n = 0;
}

/* ---- frames ---- */
static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t DESK[6]  = { 0x00, 0x40, 0xab, 0xc9, 0x91, 0x9c };
static const uint8_t BOX[6]   = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x41 };
static const uint8_t OUR[6]   = { 0x34, 0x5a, 0x60, 0x9f, 0x9e, 0xbe };

/* A wire frame around a captured [16:50] window: header, the window, silence, the
 * C2 EA end marker, at a legal geometry `len`. */
static size_t frame_of(uint8_t *out, size_t len, const uint8_t dst[6], const uint8_t src[6],
                       uint16_t counter, const uint8_t win[34])
{
	memset(out, 0, len);
	memcpy(out, dst, 6);
	memcpy(out + 6, src, 6);
	out[12] = 0x88;
	out[13] = 0x19;
	out[14] = counter & 0xff;
	out[15] = counter >> 8;
	memcpy(out + 16, win, 34);
	out[len - 2] = REAC_END_MARKER_0;
	out[len - 1] = REAC_END_MARKER_1;
	return len;
}

static size_t frame_0832(uint8_t *out, const struct box_0832_frame *f)
{
	memset(out, 0, f->len);
	memcpy(out, f->head, sizeof f->head);
	out[f->len - 2] = REAC_END_MARKER_0;
	out[f->len - 1] = REAC_END_MARKER_1;
	return f->len;
}

struct fixture {
	const char *name;
	const uint8_t *win;
	int from_desk;   /* sent by the desk (broadcast) rather than by the box (unicast) */
};

static const struct fixture FIXTURES[] = {
	{ "filler", FX_FILLER, 0 },
	{ "announce", FX_ANNOUNCE, 1 },
	{ "l1-bulk-first", FX_L1_BULK_FIRST, 1 },
	{ "l1-bulk-middle", FX_L1_BULK_MIDDLE, 1 },
	{ "l1-bulk-last", FX_L1_BULK_LAST, 1 },
	{ "l1-bulk-single", FX_L1_BULK_SINGLE, 0 },
	{ "l1-slot-map", FX_L1_SLOT_MAP, 1 },
	{ "l1-group-map", FX_L1_GROUP_MAP, 1 },
	{ "l1-box-hb", FX_L1_BOX_HB, 0 },
	{ "l1-decl-82", FX_L1_DECL_82, 0 },
	{ "l1-decl-84", FX_L1_DECL_84, 0 },
	{ "l4-head-mark", FX_L4_HEAD_MARK, 1 },
	{ "l4-join-grant", FX_L4_JOIN_GRANT, 0 },
	{ "l4-headamp", FX_L4_HEADAMP, 1 },
	{ "l4-box-ready", FX_L4_BOX_READY, 0 },
	{ "l4-identity", FX_L4_IDENTITY, 1 },
	{ "l4-return", FX_L4_RETURN, 0 },
	{ "frag-first", FX_FRAG_FIRST, 0 },
	{ "frag-last", FX_FRAG_LAST, 0 },
};
#define N_FIXTURES (sizeof FIXTURES / sizeof FIXTURES[0])

/* The fixture as the frame it arrived in: a desk broadcasts the 40-wide
 * downstream, a box unicasts its 8-wide return to us. */
static size_t fixture_frame(uint8_t *out, const struct fixture *fx, uint16_t counter)
{
	if (fx->from_desk)
		return frame_of(out, REAC_FRAME_BYTES, BCAST, DESK, counter, fx->win);
	return frame_of(out, reac_ctrl_box_frame_len(8), OUR, BOX, counter, fx->win);
}

/* A downstream frame whose 480 samples are all distinct. */
static size_t program_frame(uint8_t *out, uint16_t counter)
{
	static float planes[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
	float *planar[REAC_MAX_CHANNELS];
	for (int ch = 0; ch < REAC_MAX_CHANNELS; ch++) {
		planar[ch] = planes[ch];
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
			planes[ch][s] = (float)((ch * REAC_SAMPLES_PER_PKT + s + 1) * 1024 *
			                        ((ch & 1) ? -1 : 1)) / 8388608.0f;
	}
	return (size_t)reac_downstream_build(out, planar, REAC_MAX_CHANNELS,
	                                     REAC_SAMPLES_PER_PKT, counter, DESK);
}

static uint8_t g_frame[2048];

/* ---- frames each parser must refuse ----
 * A real frame with one thing wrong. The block checksum is re-stamped after the
 * change unless the change IS the checksum, so the refusal under test is the one
 * the breakage names and not the checksum in front of it. */
enum base_kind { B_FIXTURE, B_IDENTITY_REPLY };

struct breakage {
	const char *name;
	enum base_kind base;
	int fixture;          /* FIXTURES[] index for B_FIXTURE */
	int at;               /* frame offset to change; -1 = truncate to 49 bytes,
	                       * -2 = the SysEx's closing f7 */
	uint8_t v;            /* the byte it becomes */
	int restamp;
	const uint8_t *src;   /* a different source, or NULL */
	const uint8_t *dst;   /* a different destination, or NULL */
};

static const uint8_t STRANGER[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const uint8_t ELSEWHERE[6] = { 0x00, 0x40, 0xab, 0x99, 0x99, 0x99 };

#define BLK(k) (18 + (k))
static const struct breakage BREAKAGES[] = {
	{ "short",                B_FIXTURE, 8,  -1,        0,    0, NULL, NULL },
	{ "not-8819",             B_FIXTURE, 8,  13,        0x00, 0, NULL, NULL },
	{ "split-announce",       B_FIXTURE, 1,  16,        0xce, 1, NULL, NULL },
	{ "unknown-type",         B_FIXTURE, 1,  16,        0xab, 1, NULL, NULL },
	{ "link-2",               B_FIXTURE, 8,  BLK(0),    0x02, 1, NULL, NULL },
	{ "link-9",               B_FIXTURE, 8,  BLK(0),    0x09, 1, NULL, NULL },
	{ "l1-unknown-opcode",    B_FIXTURE, 8,  BLK(4),    0x55, 1, NULL, NULL },
	{ "bad-checksum",         B_FIXTURE, 8,  BLK(31),   0x00, 0, NULL, NULL },
	{ "not-roland",           B_FIXTURE, 8,  0,         0,    0, STRANGER, NULL },
	{ "our-echo",             B_FIXTURE, 8,  0,         0,    0, OUR, NULL },
	{ "to-somebody-else",     B_FIXTURE, 8,  0,         0,    0, NULL, ELSEWHERE },
	{ "decl-unknown-slot",    B_FIXTURE, 9,  BLK(10),   0x07, 1, NULL, NULL },
	{ "decl-no-row",          B_FIXTURE, 9,  BLK(30),   0x5a, 1, NULL, NULL },
	{ "decl-wrong-seg",       B_FIXTURE, 9,  BLK(1),    0x01, 1, NULL, NULL },
	{ "headamp-record",       B_FIXTURE, 13, BLK(20),   0x00, 1, NULL, NULL },
	{ "reply-model",          B_IDENTITY_REPLY, 0, BLK(14), 0x13, 1, NULL, NULL },
	{ "reply-tag",            B_IDENTITY_REPLY, 0, BLK(17), 0x01, 1, NULL, NULL },
	{ "reply-len-short",      B_IDENTITY_REPLY, 0, BLK(8),  13,   1, NULL, NULL },
	{ "reply-len-long",       B_IDENTITY_REPLY, 0, BLK(8),  24,   1, NULL, NULL },
	{ "reply-no-f0",          B_IDENTITY_REPLY, 0, BLK(9),  0xf1, 1, NULL, NULL },
	{ "reply-no-f7",          B_IDENTITY_REPLY, 0, -2,      0xf6, 1, NULL, NULL },
	{ "reply-outer-checksum", B_IDENTITY_REPLY, 0, BLK(31), 0x00, 0, NULL, NULL },
	{ "reply-inner-checksum", B_IDENTITY_REPLY, 0, BLK(20), 0x09, 1, NULL, NULL },
	{ "frag-short",           B_FIXTURE, 17, -1,        0,    0, NULL, NULL },
	{ "frag-seg",             B_FIXTURE, 17, BLK(1),    0x03, 1, NULL, NULL },
	{ "frag-header",          B_FIXTURE, 17, BLK(5),    0x03, 1, NULL, NULL },
	{ "frag-empty",           B_FIXTURE, 17, BLK(8),    0x00, 1, NULL, NULL },
	{ "frag-long",            B_FIXTURE, 17, BLK(8),    23,   1, NULL, NULL },
	{ "frag-checksum",        B_FIXTURE, 17, BLK(31),   0x00, 0, NULL, NULL },
	{ "frag-first-sysex",     B_FIXTURE, 17, BLK(9),    0xf1, 1, NULL, NULL },
	{ "frag-last-closes",     B_FIXTURE, 18, 0,         0,    0, NULL, NULL },
	{ "frag-first-again",     B_FIXTURE, 17, BLK(25),   0x00, 1, NULL, NULL },
	{ "frag-last-again",      B_FIXTURE, 18, 0,         0,    0, NULL, NULL },
	{ NULL, 0, 0, 0, 0, 0, NULL, NULL },
};

static size_t broken_frame(uint8_t *out, const struct breakage *b)
{
	size_t n;
	if (b->base == B_IDENTITY_REPLY) {
		static const uint8_t box[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x41 };
		n = reac_ctrl_build_as(out, reac_box_catalogue_by_token("s1608"),
		                       REAC_BOX_BLOCK_CC0016, OUR, box, 9, NULL, 0);
	} else {
		n = fixture_frame(out, &FIXTURES[b->fixture], 9);
	}
	if (b->src)
		memcpy(out + 6, b->src, 6);
	if (b->dst)
		memcpy(out, b->dst, 6);
	if (b->at == -1)
		return 49;
	if (b->at == -2)
		out[BLK(9) + out[BLK(8)] - 1] = b->v;
	else if (b->at > 0)
		out[b->at] = b->v;
	if (b->restamp)
		reac_ctrl_checksum_apply(out);
	return n;
}

/* ---- per harness ---- */
static void gen_decode(void)
{
	struct seed s = { 0 };
	static const struct { const char *name; uint8_t sel, nch, ns; } MODES[] = {
		{ "44k1", 0, 0, 0 }, { "48k", 1, 0, 0 }, { "96k", 2, 0, 0 },
		{ "custom-40x12", 3, 40, 12 }, { "custom-2x12", 3, 2, 12 },
		{ "custom-20x24", 3, 20, 24 }, { "custom-41x12", 3, 41, 12 },
	};
	char name[96];
	for (size_t m = 0; m < sizeof MODES / sizeof MODES[0]; m++) {
		put8(&s, MODES[m].sel); put8(&s, MODES[m].nch); put8(&s, MODES[m].ns);
		put(&s, g_frame, program_frame(g_frame, (uint16_t)(0x1000 + m)));
		snprintf(name, sizeof name, "downstream-program-%s", MODES[m].name);
		emit("decode", name, &s);
	}
	/* the 41x12 seed's frame (counter 0x1006) under an odd width and a width past
	 * the region: the braid's even-width rule and the geometry bound, each alone */
	static const unsigned WIDE[] = { 39, 42 };
	for (size_t i = 0; i < sizeof WIDE / sizeof WIDE[0]; i++) {
		put8(&s, 3); put8(&s, WIDE[i]); put8(&s, 12);
		put(&s, g_frame, program_frame(g_frame, 0x1006));
		snprintf(name, sizeof name, "downstream-program-custom-%ux12", WIDE[i]);
		emit("decode", name, &s);
	}
	/* the plain silent downstream and its FCS-residue twin */
	put8(&s, 1); put8(&s, 0); put8(&s, 0);
	put(&s, g_frame, (size_t)reac_downstream_build(g_frame, NULL, 0, 0, 7, DESK));
	emit("decode", "downstream-silent", &s);
	put8(&s, 1); put8(&s, 0); put8(&s, 0);
	put(&s, g_frame, (size_t)reac_downstream_build(g_frame, NULL, 0, 0, 7, DESK));
	put8(&s, 0x12); put8(&s, 0x34);
	emit("decode", "downstream-fcs-residue", &s);

	static const struct { const char *name; const uint8_t *p; size_t n; } UP[] = {
		{ "upstream-s1608-16ch", UP16, sizeof UP16 },
		{ "upstream-s0808-8ch", UP8, sizeof UP8 },
		{ "upstream-32ch-a", UP32A, sizeof UP32A },
		{ "upstream-32ch-b", UP32B, sizeof UP32B },
	};
	for (size_t i = 0; i < sizeof UP / sizeof UP[0]; i++) {
		put8(&s, 1); put8(&s, 0); put8(&s, 0);
		put(&s, UP[i].p, UP[i].n);
		emit("decode", UP[i].name, &s);
	}
	for (size_t i = 0; i < BOX_0832_N; i++) {
		put8(&s, 1); put8(&s, 0); put8(&s, 0);
		put(&s, g_frame, frame_0832(g_frame, &BOX_0832_FRAMES[i]));
		snprintf(name, sizeof name, "box-0832-%02zu-%u", i, BOX_0832_FRAMES[i].len);
		emit("decode", name, &s);
	}
	/* each refusal: a wrong EtherType, a wrong end marker, a width no box has */
	static const struct { const char *name; size_t at; uint8_t v; } BREAK[] = {
		{ "ethertype", 13, 0x18 }, { "end-marker", (size_t)-1, 0xeb },
	};
	for (size_t i = 0; i < 2; i++) {
		size_t n = program_frame(g_frame, 3);
		g_frame[BREAK[i].at == (size_t)-1 ? n - 1 : BREAK[i].at] = BREAK[i].v;
		put8(&s, 1); put8(&s, 0); put8(&s, 0);
		put(&s, g_frame, n);
		snprintf(name, sizeof name, "downstream-bad-%s", BREAK[i].name);
		emit("decode", name, &s);
		memcpy(g_frame, UP8, sizeof UP8);
		g_frame[BREAK[i].at == (size_t)-1 ? sizeof UP8 - 1 : BREAK[i].at] = BREAK[i].v;
		put8(&s, 1); put8(&s, 0); put8(&s, 0);
		put(&s, g_frame, sizeof UP8);
		snprintf(name, sizeof name, "upstream-bad-%s", BREAK[i].name);
		emit("decode", name, &s);
	}
	static const unsigned WIDTHS[] = { 3, 42 };
	for (size_t i = 0; i < 2; i++) {
		size_t n = REAC_UPSTREAM_OVERHEAD + WIDTHS[i] * REAC_UPSTREAM_BYTES_PER_CH;
		memset(g_frame, 0, n);
		memcpy(g_frame, UP8, 50);
		g_frame[n - 2] = REAC_END_MARKER_0;
		g_frame[n - 1] = REAC_END_MARKER_1;
		put8(&s, 1); put8(&s, 0); put8(&s, 0);
		put(&s, g_frame, n);
		snprintf(name, sizeof name, "upstream-width-%u", WIDTHS[i]);
		emit("decode", name, &s);
	}
	free(s.p);
}

static void gen_ctrl(void)
{
	struct seed s = { 0 };
	char name[96];
	for (size_t i = 0; i < N_FIXTURES; i++) {
		put(&s, OUR, 6); put8(&s, 0);
		put_rec(&s, g_frame, fixture_frame(g_frame, &FIXTURES[i], (uint16_t)i));
		snprintf(name, sizeof name, "fixture-%s", FIXTURES[i].name);
		emit("ctrl", name, &s);
	}
	/* every fixture in one conversation; the two fragments arrive in order */
	put(&s, OUR, 6); put8(&s, 0);
	for (size_t i = 0; i < N_FIXTURES; i++)
		put_rec(&s, g_frame, fixture_frame(g_frame, &FIXTURES[i], (uint16_t)i));
	emit("ctrl", "fixtures-all", &s);
	/* the S-4000H-0832's whole first second on VLAN 13 */
	put(&s, BOX_0832_OUR, 6); put8(&s, 0);
	for (size_t i = 0; i < BOX_0832_N; i++)
		put_rec(&s, g_frame, frame_0832(g_frame, &BOX_0832_FRAMES[i]));
	emit("ctrl", "box-0832-session", &s);
	/* the builder round trip, once per catalogue row */
	size_t count = 0;
	const struct reac_box_model *cat = reac_box_catalogue(&count);
	for (size_t i = 0; i < count; i++) {
		put(&s, OUR, 6); put8(&s, (unsigned)(i << 1 | 1));
		snprintf(name, sizeof name, "round-trip-%s", cat[i].token);
		emit("ctrl", name, &s);
	}
	/* the two single-record identity replies a box answers a cold connect with, whole:
	 * its firmware (address 0x0000) and its REAC version (0x0600), each its own seed */
	static const struct { const char *name; enum reac_box_block block; } REPLIES[] = {
		{ "reply-firmware",     REAC_BOX_BLOCK_CC0016 },
		{ "reply-reac-version", REAC_BOX_BLOCK_CC001A },
		/* the firmware reply again, under the name the enrolment review's records
		 * give it */
		{ "identity-reply-s1608-firmware", REAC_BOX_BLOCK_CC0016 },
	};
	for (size_t i = 0; i < sizeof REPLIES / sizeof REPLIES[0]; i++) {
		uint16_t addr;
		const uint8_t *pl;
		size_t pll;
		size_t n = reac_ctrl_build_as(g_frame, reac_box_catalogue_by_token("s1608"),
		                              REPLIES[i].block, OUR, BOX, 9, NULL, 0);
		if (n == 0 || reac_ctrl_identity_reply(g_frame, n, &addr, &pl, &pll) != 1) {
			fprintf(stderr, "gen-corpus: %s is not a single-record identity reply\n",
			        REPLIES[i].name);
			exit(1);
		}
		put(&s, OUR, 6); put8(&s, 0);
		put_rec(&s, g_frame, n);
		emit("ctrl", REPLIES[i].name, &s);
	}
	/* every refusal the parsers make, one breakage per frame */
	put(&s, OUR, 6); put8(&s, 0);
	for (const struct breakage *b = BREAKAGES; b->name; b++) {
		size_t n = broken_frame(g_frame, b);
		put_rec(&s, g_frame, n);
	}
	emit("ctrl", "breakages", &s);
	/* the reply whose DT1 record sum does not close, alone: the block sum closes, so
	 * only the record check refuses it */
	for (const struct breakage *b = BREAKAGES; b->name; b++) {
		if (strcmp(b->name, "reply-inner-checksum") != 0)
			continue;
		put(&s, OUR, 6); put8(&s, 0);
		put_rec(&s, g_frame, broken_frame(g_frame, b));
		emit("ctrl", b->name, &s);
	}
	/* the box heartbeat under type 00 01, its block sum broken: not FILLER, so
	 * the classifier must refuse it */
	{
		size_t n = fixture_frame(g_frame, &FIXTURES[8], 8);
		g_frame[16] = 0x00;
		g_frame[17] = 0x01;
		g_frame[BLK(31)] = 0x00;
		put(&s, OUR, 6); put8(&s, 0);
		put_rec(&s, g_frame, n);
		emit("ctrl", "box-hb-type-0001-bad-checksum", &s);
	}
	/* a declaration with two slot codes nobody has captured: the first one is named */
	{
		size_t n = fixture_frame(g_frame, &FIXTURES[9], 9);
		g_frame[BLK(10)] = 0x07;
		g_frame[BLK(11)] = 0x09;
		reac_ctrl_checksum_apply(g_frame);
		put(&s, OUR, 6); put8(&s, 0);
		put_rec(&s, g_frame, n);
		emit("ctrl", "decl-two-unknown-slots", &s);
	}
	/* the S-0808 name record whose FIRST fragment carries a changed name byte under
	 * a restamped block: both block sums close, the record sum does not */
	{
		size_t n = fixture_frame(g_frame, &FIXTURES[17], 9);
		g_frame[BLK(25)] = 0x00;
		reac_ctrl_checksum_apply(g_frame);
		put(&s, OUR, 6); put8(&s, 0);
		put_rec(&s, g_frame, n);
		put_rec(&s, g_frame, fixture_frame(g_frame, &FIXTURES[18], 9));
		emit("ctrl", "frag-inner-checksum", &s);
	}
	/* the identity record's fragments out of order and from two boxes */
	put(&s, OUR, 6); put8(&s, 0);
	put_rec(&s, g_frame, fixture_frame(g_frame, &FIXTURES[18], 1));   /* LAST alone */
	put_rec(&s, g_frame, fixture_frame(g_frame, &FIXTURES[17], 2));   /* FIRST */
	put_rec(&s, g_frame, frame_of(g_frame, reac_ctrl_box_frame_len(8), OUR, DESK, 3,
	                              FX_FRAG_LAST));                     /* LAST, other box */
	emit("ctrl", "fragments-mismatched", &s);
	/* our own heartbeat, heard back: we are the box this time */
	put(&s, BOX, 6); put8(&s, 0);
	put_rec(&s, g_frame, fixture_frame(g_frame, &FIXTURES[8], 1));
	emit("ctrl", "own-echo", &s);
	/* and our own join grant heard back */
	put(&s, BOX, 6); put8(&s, 0);
	put_rec(&s, g_frame, fixture_frame(g_frame, &FIXTURES[12], 12));
	emit("ctrl", "own-echo-join", &s);
	free(s.p);
}

/* An identity record as fuzz_identity reads it. addr_lo is biased by its top two
 * bits: 00 firmware, 01 REAC version, 10 model name, 11 the address as given. */
static void put_ident(struct seed *s, unsigned sel, unsigned addr, const uint8_t *p, size_t n)
{
	put16(s, sel << 14 | (addr & 0x3fff));
	put8(s, (unsigned)n);
	put(s, p, n);
}

/* The tail at chosen name, token and display capacities. */
static void put_tail_caps(struct seed *s, unsigned fam, unsigned in, unsigned out,
                          unsigned cap_name, unsigned cap_tok, unsigned cap_disp)
{
	put8(s, fam); put8(s, in); put8(s, out);
	put8(s, 15); put8(s, 31); put8(s, cap_name); put8(s, cap_tok); put8(s, cap_disp);
	put8(s, 0);
	put16(s, 10234);   /* a firmware of 10.234: a two-digit whole part */
}

static void put_name_tail(struct seed *s, unsigned fam, unsigned in, unsigned out)
{
	put_tail_caps(s, fam, in, out, 39, 39, 63);
}

static void gen_identity(void)
{
	struct seed s = { 0 };
	char name[96];
	size_t count = 0;
	const struct reac_box_model *cat = reac_box_catalogue(&count);
	uint8_t out[REAC_FRAME_BYTES];
	static const uint8_t box[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };
	/* each catalogue row's identity page, lifted out of its own frames by the
	 * library's own reply parser and fragment reassembly */
	for (size_t i = 0; i < count; i++) {
		struct reac_identity_frag frag;
		memset(&frag, 0, sizeof frag);
		struct seed recs = { 0 };
		unsigned nrec = 0;
		for (int b = REAC_BOX_BLOCK_CC0016; b <= REAC_BOX_BLOCK_IDENT_LAST; b++) {
			size_t n = reac_ctrl_build_as(out, &cat[i], (enum reac_box_block)b, OUR, box,
			                              1, NULL, 0);
			uint16_t addr;
			const uint8_t *pl;
			size_t pll;
			if (n == 0)
				continue;
			int r = (b == REAC_BOX_BLOCK_IDENT_FIRST || b == REAC_BOX_BLOCK_IDENT_LAST)
			      ? reac_ctrl_identity_fragment(&frag, out, n, &addr, &pl, &pll)
			      : reac_ctrl_identity_reply(out, n, &addr, &pl, &pll);
			if (r != 1)
				continue;
			unsigned sel = addr == REAC_IDENTITY_ADDR_FIRMWARE ? 0
			             : addr == REAC_IDENTITY_ADDR_REAC_VERSION ? 1
			             : addr == REAC_IDENTITY_ADDR_MODEL_NAME ? 2 : 3;
			put_ident(&recs, sel, addr, pl, pll);
			nrec++;
		}
		put8(&s, nrec);
		put(&s, recs.p, recs.n);
		free(recs.p);
		put_name_tail(&s, 0, (unsigned)cat[i].in_ch, (unsigned)cat[i].out_ch);
		snprintf(name, sizeof name, "catalogue-%s", cat[i].token);
		emit("identity", name, &s);
	}
	/* the captured S-0808 name record, reassembled from the two fixture fragments */
	{
		struct reac_identity_frag frag;
		memset(&frag, 0, sizeof frag);
		uint16_t addr;
		const uint8_t *pl;
		size_t pll;
		struct fixture first = { "", FX_FRAG_FIRST, 0 }, last = { "", FX_FRAG_LAST, 0 };
		size_t n = fixture_frame(g_frame, &first, 1);
		reac_ctrl_identity_fragment(&frag, g_frame, n, &addr, &pl, &pll);
		n = fixture_frame(g_frame, &last, 2);
		if (reac_ctrl_identity_fragment(&frag, g_frame, n, &addr, &pl, &pll) == 1) {
			put8(&s, 1);
			put_ident(&s, 2, addr, pl, pll);
			put_name_tail(&s, 0, 8, 8);
			emit("identity", "fixture-s0808-name", &s);
		}
	}
	/* the three firmware and REAC-version replies the identity header quotes */
	static const uint8_t FW[3][4] = { { 1, 0, 0, 3 }, { 2, 2, 0, 0 }, { 2, 5, 0, 0 } };
	static const uint8_t VER[3][8] = { { 0, 0, 0, 1, 0, 0, 0, 0 },
	                                   { 0, 0, 0, 2, 0, 3, 0, 2 },
	                                   { 0, 0, 0, 2, 0, 1, 0, 2 } };
	static const unsigned W[3][2] = { { 8, 8 }, { 16, 8 }, { 32, 8 } };
	for (int i = 0; i < 3; i++) {
		put8(&s, 2);
		put_ident(&s, 0, 0, FW[i], 4);
		put_ident(&s, 1, 0, VER[i], 8);
		put_name_tail(&s, 0, W[i][0], W[i][1]);
		snprintf(name, sizeof name, "header-example-%d", i);
		emit("identity", name, &s);
	}
	/* every reply the decode refuses, then names at and past the field's width */
	static const uint8_t BAD_FW[4] = { 1, 0, 10, 3 };
	static const uint8_t NUL_NAME[4] = { 1, 0, 0, 0 };
	static const uint8_t LONG_NAME[21] = { 1, 'S', '-', '4', '0', '0', '0', 'S', '-', '3',
	                                       '2', '0', '8', '-', 'R', 'A', 'C', 'K', 'X', 'Y',
	                                       'Z' };
	put8(&s, 8);
	put_ident(&s, 0, 0, FW[0], 3);
	put_ident(&s, 0, 0, BAD_FW, 4);
	put_ident(&s, 1, 0, VER[1], 7);
	put_ident(&s, 2, 0, NUL_NAME, 1);
	put_ident(&s, 2, 0, NUL_NAME, 4);
	put_ident(&s, 3, 0x1011, FW[1], 4);
	put_ident(&s, 2, 0, LONG_NAME, sizeof LONG_NAME);
	put_ident(&s, 0, 0, FW[1], 4);
	/* a name too long for every buffer, at capacities too small for anything */
	put8(&s, 0x81); put8(&s, 32); put8(&s, 8);
	put8(&s, 7); put8(&s, 19); put8(&s, 5); put8(&s, 5); put8(&s, 9); put8(&s, 3);
	put16(&s, 12345);
	emit("identity", "refusals-and-small-buffers", &s);
	/* a width no box has */
	put8(&s, 1);
	put_ident(&s, 1, 0, VER[2], 8);
	put_name_tail(&s, 0, 3, 8);
	emit("identity", "odd-width", &s);
	/* a name of two bytes and no NUL: the decode must stop at the payload's end */
	static const uint8_t NAME_AB[3] = { 1, 'A', 'B' };
	put8(&s, 1);
	put_ident(&s, 2, 0, NAME_AB, sizeof NAME_AB);
	emit("identity", "name-no-nul-sabotage", &s);
	/* the S-1608's page, each of its three strings into a buffer of exactly its
	 * length: "S-1608" (6), "s1608" (5), "S-1608 (16 in / 8 out)" (22), no room for
	 * the NUL */
	static const struct { const char *name; unsigned nm, tk, dp; } EXACT[] = {
		{ "s1608-name-cap-equal-length",    6, 39, 63 },
		{ "s1608-token-cap-equal-length",  39,  5, 63 },
		{ "s1608-display-cap-equal-length", 39, 39, 22 },
	};
	for (size_t i = 0; i < sizeof EXACT / sizeof EXACT[0]; i++) {
		put8(&s, 2);
		put_ident(&s, 0, 0, FW[1], 4);
		put_ident(&s, 1, REAC_IDENTITY_ADDR_REAC_VERSION, VER[1], 8);
		put_tail_caps(&s, 0, 16, 8, EXACT[i].nm, EXACT[i].tk, EXACT[i].dp);
		emit("identity", EXACT[i].name, &s);
	}
	free(s.p);
}

static void put_op(struct seed *s, unsigned op, unsigned dt_ms)
{
	put8(s, op);
	put16(s, dt_ms);
}

static void gen_disco(void)
{
	struct seed s = { 0 };
	char name[96];
	/* each fixture through every door */
	for (size_t i = 0; i < N_FIXTURES; i++) {
		put(&s, OUR, 6); put8(&s, 0);
		for (unsigned op = 0; op < 3; op++) {
			put_op(&s, op, 1);
			put_rec(&s, g_frame, fixture_frame(g_frame, &FIXTURES[i], (uint16_t)i));
		}
		put_op(&s, 3, 10);
		snprintf(name, sizeof name, "fixture-%s", FIXTURES[i].name);
		emit("disco", name, &s);
	}
	/* a desk and a box on one segment, the hunt hearing both and then losing them */
	put(&s, OUR, 6); put8(&s, 0);
	for (size_t i = 0; i < N_FIXTURES; i++) {
		put_op(&s, 2, 5);
		put_rec(&s, g_frame, fixture_frame(g_frame, &FIXTURES[i], (uint16_t)i));
		put_op(&s, 1, 0);
		put_rec(&s, g_frame, fixture_frame(g_frame, &FIXTURES[i], (uint16_t)i));
	}
	put_op(&s, 3, 1000);
	put_op(&s, 3, 6000);
	put_op(&s, 3 | 0x80, 3000);
	emit("disco", "desk-and-box", &s);
	/* the S-4000H-0832 session through the hunt, pinned and not */
	for (unsigned setup = 0; setup < 4; setup += 3) {
		put(&s, BOX_0832_OUR, 6); put8(&s, setup);
		for (size_t i = 0; i < BOX_0832_N; i++) {
			put_op(&s, 2, 1);
			put_rec(&s, g_frame, frame_0832(g_frame, &BOX_0832_FRAMES[i]));
		}
		put_op(&s, 3, 100);
		put_op(&s, 3, 4000);
		snprintf(name, sizeof name, "box-0832-hunt-setup%u", setup);
		emit("disco", name, &s);
	}
	/* a full table: eight boxes flooding, one after the other */
	put(&s, OUR, 6); put8(&s, 0);
	for (unsigned b = 0; b < 8; b++) {
		uint8_t src[6] = { 0x00, 0x40, 0xab, 0xc4, 0x00, (uint8_t)(0x10 + b) };
		put_op(&s, 2 | (b & 1 ? 4 : 0), 1);
		put_rec(&s, g_frame, frame_of(g_frame, reac_ctrl_box_frame_len(8), BCAST, src,
		                              (uint16_t)b, FX_FILLER));
	}
	put_op(&s, 3, 0);
	emit("disco", "eight-boxes", &s);
	/* nine peers, straight into the table and through the gate: both saturate; the
	 * second seed then serialises the full table into REAC_DISCO_JSON_MAX */
	for (int full = 0; full < 2; full++) {
		put(&s, OUR, 6); put8(&s, 0);
		for (unsigned b = 0; b < 9; b++) {
			uint8_t src[6] = { 0x00, 0x40, 0xab, 0xc4, 0x01, (uint8_t)(0x20 + b) };
			put_op(&s, 0 | 4 | 8 | 0x10 | (b % 3) << 5, 1);
			put_rec(&s, g_frame, frame_of(g_frame, reac_ctrl_box_frame_len(8), OUR, src,
			                              (uint16_t)b, FX_FILLER));
		}
		put_op(&s, 3 | 1 << 2, 0);   /* a 32-byte buffer: too small, refused */
		put_op(&s, 3 | 2 << 2, 0);
		if (full)
			put_op(&s, 3, 0);
		emit("disco", full ? "nine-peers-full-json" : "nine-peers", &s);
	}
	/* the peer lock, the gate's edges and its refresh window */
	put(&s, OUR, 6); put8(&s, 4);
	put_op(&s, 1, 1);    /* a verified heartbeat from the box latches the lock */
	put_rec(&s, g_frame, fixture_frame(g_frame, &FIXTURES[8], 1));
	put_op(&s, 1, 1);    /* a FILLER claiming to be the desk is then refused */
	put_rec(&s, g_frame, frame_of(g_frame, REAC_FRAME_BYTES, BCAST, DESK, 2, FX_FILLER));
	put_op(&s, 0, 1);    /* a heartbeat whose block checksum does not close */
	{
		size_t n = fixture_frame(g_frame, &FIXTURES[8], 3);
		g_frame[BLK(31)] ^= 0x01;
		put_rec(&s, g_frame, n);
	}
	put_op(&s, 1, 1);    /* our own echo */
	put_rec(&s, g_frame, frame_of(g_frame, REAC_FRAME_BYTES, BCAST, OUR, 3, FX_ANNOUNCE));
	put_op(&s, 2, 1);    /* not 0x8819, to the hunt */
	{
		size_t n = fixture_frame(g_frame, &FIXTURES[8], 4);
		g_frame[13] = 0x00;
		put_rec(&s, g_frame, n);
	}
	for (unsigned slots = 0x28; slots <= 0x30; slots += 8) {
		uint8_t win[34];
		memcpy(win, FX_ANNOUNCE, 34);
		win[2 + 15] = (uint8_t)slots;
		reac_ctrl_block_cksum_stamp(win + 2);
		put_op(&s, 0, 1);
		put_rec(&s, g_frame, frame_of(g_frame, REAC_FRAME_BYTES, BCAST, DESK, 5, win));
	}
	put_op(&s, 0, 1500);  /* the same frame after the refresh window */
	put_rec(&s, g_frame, fixture_frame(g_frame, &FIXTURES[1], 6));
	put_op(&s, 0, 100);   /* and inside it */
	put_rec(&s, g_frame, fixture_frame(g_frame, &FIXTURES[1], 7));
	put_op(&s, 0, 1500);  /* and after the window again, with nothing new to say */
	put_rec(&s, g_frame, fixture_frame(g_frame, &FIXTURES[1], 8));
	/* the same box, now ours and named by its identity page */
	put_op(&s, 0 | 4 | 8, 1);
	put_rec(&s, g_frame, fixture_frame(g_frame, &FIXTURES[8], 9));
	put_op(&s, 0 | 4 | 0x10 | 2 << 5, 1);
	put_rec(&s, g_frame, fixture_frame(g_frame, &FIXTURES[8], 10));
	put_op(&s, 3 | 1 << 2, 10);   /* a 2-byte buffer: refused */
	emit("disco", "lock-and-gate", &s);
	/* a box flooding broadcast and nothing else: after the hunt's window it is a box */
	put(&s, OUR, 6); put8(&s, 0);
	for (unsigned i = 0; i < 4; i++) {
		put_op(&s, 2, 1);
		put_rec(&s, g_frame, frame_of(g_frame, reac_ctrl_box_frame_len(8), BCAST, BOX,
		                              (uint16_t)i, FX_FILLER));
	}
	put_op(&s, 3, 1000);
	put_op(&s, 3, 2500);
	emit("disco", "flood-only", &s);
	/* a desk's 40-wide downstream before its announce: the hunt holds */
	put(&s, OUR, 6); put8(&s, 0);
	put_op(&s, 2, 1);
	put_rec(&s, g_frame, frame_of(g_frame, REAC_FRAME_BYTES, BCAST, DESK, 1, FX_FILLER));
	put_op(&s, 3, 1);
	emit("disco", "desk-geometry", &s);
	/* a box strapped to master, announcing at its own width, on a wire pinned master */
	for (unsigned setup = 0; setup < 4; setup += 3) {
		put(&s, OUR, 6); put8(&s, setup);
		put_op(&s, 2, 1);
		put_rec(&s, g_frame, frame_of(g_frame, reac_ctrl_box_frame_len(16), BCAST, BOX, 1,
		                              FX_ANNOUNCE));
		put_op(&s, 3, 1);
		snprintf(name, sizeof name, "box-on-m-setup%u", setup);
		emit("disco", name, &s);
	}
	/* a master announce at no legal geometry: a rival nobody has captured */
	put(&s, OUR, 6); put8(&s, 0);
	put_op(&s, 2, 1);
	put_rec(&s, g_frame, frame_of(g_frame, 600, BCAST, DESK, 1, FX_ANNOUNCE));
	put_op(&s, 3, 1);
	emit("disco", "unreadable-rival", &s);
	/* a box FILLER (type 00 00, an all-zero block) one byte short of a control block */
	put(&s, OUR, 6); put8(&s, 0);
	put_op(&s, 0, 1);
	{
		static const uint8_t ZERO_WIN[34];
		frame_of(g_frame, reac_ctrl_box_frame_len(8), OUR, BOX, 0, ZERO_WIN);
		put_rec(&s, g_frame, 49);
	}
	emit("disco", "short-filler-49", &s);
	/* the box heartbeat with the two FCS bytes a capture keeps, to the hunt */
	put(&s, OUR, 6); put8(&s, 0);
	put_op(&s, 2, 1);
	{
		size_t n = fixture_frame(g_frame, &FIXTURES[8], 8);
		g_frame[n] = 0x00;
		g_frame[n + 1] = 0x00;
		put_rec(&s, g_frame, n + 2);
	}
	emit("disco", "box-hb-fcs-hunt", &s);
	/* the box heartbeat into the table, then an age with the clock stepped back */
	put(&s, OUR, 6); put8(&s, 0);
	put_op(&s, 0, 1);
	put_rec(&s, g_frame, fixture_frame(g_frame, &FIXTURES[8], 8));
	put_op(&s, 3 | 0x80, 1);
	emit("disco", "box-then-clock-back", &s);
	/* a box's FILLER, then its link-1 bulk SINGLE, both against the peer lock */
	put(&s, OUR, 6); put8(&s, 0);
	put_op(&s, 1, 5);
	put_rec(&s, g_frame, fixture_frame(g_frame, &FIXTURES[0], 0));
	put_op(&s, 1, 25);
	put_rec(&s, g_frame, fixture_frame(g_frame, &FIXTURES[5], 5));
	emit("disco", "box-then-bye-table", &s);
	free(s.p);
}

static void gen_scene(void)
{
	struct seed s = { 0 };
	char name[96];
	static const uint8_t MAC[6] = { 0x00, 0x40, 0xab, 0x01, 0x02, 0x03 };
	/* the shipped body, the whole transfer */
	put8(&s, 3); put16(&s, 0); put16(&s, 0); put(&s, MAC, 6);
	emit("scene", "placeholder-whole-transfer", &s);
	/* one step of each segment state, at the declared length */
	static const int STEPS[] = { 0, 1, 32, 33, 341, 342, 343, -1 };
	for (size_t i = 0; i < sizeof STEPS / sizeof STEPS[0]; i++) {
		put8(&s, 0); put16(&s, REAC_SCENE_BYTES); put16(&s, (unsigned)STEPS[i] & 0xffff);
		put(&s, MAC, 6);
		snprintf(name, sizeof name, "placeholder-step-%d", STEPS[i]);
		emit("scene", name, &s);
	}
	/* bodies of the wrong length */
	static const unsigned LENS[] = { 0, 24, 8903, 8905 };
	for (size_t i = 0; i < sizeof LENS / sizeof LENS[0]; i++) {
		put8(&s, 0); put16(&s, LENS[i]); put16(&s, 5); put(&s, MAC, 6);
		snprintf(name, sizeof name, "short-body-%u", LENS[i]);
		emit("scene", name, &s);
	}
	/* a body that drifts from the shipped one */
	put8(&s, 3); put16(&s, 0); put16(&s, 0); put(&s, MAC, 6);
	for (int i = 0; i < 64; i++)
		put8(&s, (unsigned)(i * 37));
	emit("scene", "drifted-body", &s);
	free(s.p);
}

static void gen_boxreg(void)
{
	struct seed s = { 0 };
	/* a desk pre-declares an S-1608 and an S-0808, then they join */
	put8(&s, 40);
	put8(&s, 0); put8(&s, 16); put8(&s, 0); put8(&s, 5); put(&s, "Drums", 5);
	put8(&s, 0); put8(&s, 8); put8(&s, 0xff); put8(&s, 4); put(&s, "Keys", 4);
	put8(&s, 1); put8(&s, 0x41); put8(&s, 16);
	put8(&s, 1); put8(&s, 0x80); put8(&s, 8);
	put8(&s, 1); put8(&s, 0x41); put8(&s, 16);
	put8(&s, 2); put8(&s, 0x41);
	put8(&s, 3); put8(&s, 1); put8(&s, 6); put(&s, "Strings", 6);
	emit("boxreg", "declare-then-join", &s);
	/* five S-0808s fill the fabric, and a sixth is refused */
	put8(&s, 40);
	for (unsigned b = 0; b < 6; b++) {
		put8(&s, 1); put8(&s, 0x10 + b); put8(&s, 8);
	}
	emit("boxreg", "fabric-full", &s);
	/* declarations that collide, a zero MAC, a narrow fabric */
	put8(&s, 24);
	put8(&s, 0); put8(&s, 16); put8(&s, 4); put8(&s, 0);
	put8(&s, 0); put8(&s, 8); put8(&s, 8); put8(&s, 0);
	put8(&s, 5); put(&s, "\0\0\0\0\0\0", 6); put8(&s, 8);
	put8(&s, 0x08); put8(&s, 4); put8(&s, 0xff); put8(&s, 0);
	put8(&s, 3); put8(&s, 0); put8(&s, 47);
	for (int i = 0; i < 47; i++)
		put8(&s, 'a' + i % 26);
	emit("boxreg", "collisions", &s);
	/* six declarations for five slots, a box too wide for what is left, a bad width */
	put8(&s, 40);
	for (unsigned b = 0; b < 6; b++) {
		put8(&s, 0); put8(&s, 4); put8(&s, 0xff); put8(&s, 0);
	}
	put8(&s, 0); put8(&s, 40); put8(&s, 0xff); put8(&s, 0);
	put8(&s, 1); put8(&s, 0x31); put8(&s, 7);
	put8(&s, 1); put8(&s, 0x32); put8(&s, 4);
	put8(&s, 1); put8(&s, 0x33); put8(&s, 6);
	put8(&s, 3); put8(&s, 9); put8(&s, 1); put8(&s, 'x');
	emit("boxreg", "no-room", &s);
	put8(&s, 16);
	put8(&s, 1); put8(&s, 0x01); put8(&s, 16);
	put8(&s, 1); put8(&s, 0x02); put8(&s, 2);
	put8(&s, 0); put8(&s, 2); put8(&s, 0xff); put8(&s, 0);
	emit("boxreg", "fabric-16-full", &s);
	/* one call each, on a 40-slot fabric: the refusals and the edges of the rules */
	static const struct { const char *name; uint8_t n, ops[12]; } ONE[] = {
		{ "add-odd-width",      3, { 1, 0x31, 7 } },
		{ "add-zero-width",     3, { 1, 0x31, 0 } },
		{ "add-zero-mac",       8, { 5, 0, 0, 0, 0, 0, 0, 8 } },
		{ "declare-odd-width",  4, { 0, 7, 0xff, 0 } },
		{ "find-absent",        2, { 2, 0x99 } },
		{ "find-near-mac",      5, { 1, 0x41, 8, 2, 0x42 } },
		{ "find-zero-mac",     11, { 0, 4, 0xff, 0, 6, 0, 0, 0, 0, 0, 0 } },
		{ "pinned-at-end",      4, { 0, 8, 32, 0 } },
		{ "pinned-then-join",   7, { 0, 16, 4, 0, 1, 0x41, 16 } },
		{ "rejoin-other-width", 6, { 1, 0x41, 16, 1, 0x41, 8 } },
	};
	for (size_t i = 0; i < sizeof ONE / sizeof ONE[0]; i++) {
		put8(&s, 40);
		put(&s, ONE[i].ops, ONE[i].n);
		emit("boxreg", ONE[i].name, &s);
	}
	free(s.p);
}

static void gen_knock(void)
{
	struct seed s = { 0 };
	/* a silent wire: steps of ~1 ms, ~77 us and ~8.4 ms until the licence */
	put32(&s, 1000);
	for (int i = 0; i < 10; i++) {
		put8(&s, 0x02 | 4 << 4); put16(&s, 0xffff);   /* 65535 << 4 ns ~ 1 ms */
	}
	for (int i = 0; i < 10; i++) {
		put8(&s, 0x02 | 7 << 4); put16(&s, 600);      /* 600 << 7 ns ~ 77 us */
	}
	put8(&s, 0x02 | 7 << 4); put16(&s, 0xffff);
	for (int i = 0; i < 64; i++) {
		put8(&s, 0x02 | 7 << 4); put16(&s, 0xffff);   /* 65535 << 7 ns ~ 8.4 ms each */
	}
	emit("knock", "silent-wire", &s);
	/* a stray frame cancels, the wire re-opens from it, and drives later */
	put32(&s, 0);
	put8(&s, 0x03 | 7 << 4); put16(&s, 0x1000);
	for (int i = 0; i < 80; i++) {
		put8(&s, 0x02 | 7 << 4); put16(&s, 0xffff);
	}
	put8(&s, 0x01 | 7 << 4); put16(&s, 1);
	emit("knock", "stray-frame", &s);
	/* a desk that never stops talking */
	put32(&s, 77);
	for (int i = 0; i < 200; i++) {
		put8(&s, 0x03 | 7 << 4); put16(&s, 0x4000);
	}
	emit("knock", "desk-talking", &s);
	/* steps of 65535 << 7 ns from a start of 0: 59 of them and 0x9b05 << 7 ns more
	 * are exactly REAC_KNOCK_LISTEN_NS (500 ms) */
	put32(&s, 0);
	for (int i = 0; i < 59; i++) {
		put8(&s, 0x02 | 7 << 4); put16(&s, 0xffff);
	}
	put8(&s, 0x02 | 7 << 4); put16(&s, 0x9b05);
	emit("knock", "exact-window", &s);
	/* a frame at 128 ns, then the window measured from it, to the nanosecond */
	put32(&s, 0);
	put8(&s, 0x01 | 7 << 4); put16(&s, 1);
	for (int i = 0; i < 59; i++) {
		put8(&s, 0x02 | 7 << 4); put16(&s, 0xffff);
	}
	put8(&s, 0x02 | 7 << 4); put16(&s, 0x9b05);
	emit("knock", "exact-reopen", &s);
	/* the clock alone past the due time, then a frame and a step in the same instant */
	put32(&s, 0);
	for (int i = 0; i < 60; i++) {
		put8(&s, 0x00 | 7 << 4); put16(&s, 0xffff);
	}
	put8(&s, 0x03 | 7 << 4); put16(&s, 0);
	emit("knock", "frame-at-due", &s);
	/* a start at 2^53 ns, stepped through a whole window */
	put32(&s, 0x00400000);
	for (int i = 0; i < 64; i++) {
		put8(&s, 0x02 | 7 << 4); put16(&s, 0xffff);
	}
	emit("knock", "late-start", &s);
	free(s.p);
}

static void gen_grant(void)
{
	struct seed s = { 0 };
	char name[96];
	/* the widths and straps the catalogue's boxes announce */
	static const struct { int base, w; } BOXES[] = {
		{ 0x00, 8 }, { 0x20, 16 }, { 0x00, 32 }, { 0x00, 40 }, { 0x10, 24 },
		{ 0x20, 32 }, { 0x2f, 1 }, { -1, 8 },
	};
	for (size_t i = 0; i < sizeof BOXES / sizeof BOXES[0]; i++) {
		put16(&s, (unsigned)BOXES[i].base & 0xffff); put8(&s, (unsigned)BOXES[i].w);
		put8(&s, 127);
		put8(&s, 3);
		put8(&s, (unsigned)(BOXES[i].base + 1) & 0xff); put8(&s, REAC_HEADAMP_PHANTOM);
		put8(&s, 1);
		put8(&s, (unsigned)(BOXES[i].base + 1) & 0xff); put8(&s, REAC_HEADAMP_SENS);
		put8(&s, 0x20);
		put8(&s, 47); put8(&s, REAC_HEADAMP_PAD); put8(&s, 1);
		/* the same slots as a hand-built allocation, and as the scene replay the
		 * pacer arms on establishment (none for a box that was refused) */
		put8(&s, (unsigned)BOXES[i].base & 0xff); put8(&s, (unsigned)BOXES[i].w);
		put8(&s, BOXES[i].base < 0 ? 0 : (unsigned)BOXES[i].base);
		put8(&s, BOXES[i].base < 0 ? 0 : (unsigned)BOXES[i].w);
		put32(&s, 64); put16(&s, 1024);
		snprintf(name, sizeof name, "box-base%d-w%d", BOXES[i].base, BOXES[i].w);
		emit("grant", name, &s);
	}
	/* cells the table refuses: a parameter that does not exist, values out of range */
	put16(&s, 0); put8(&s, 8); put8(&s, 127);
	put8(&s, 4);
	put8(&s, 1); put8(&s, 5); put8(&s, 0);
	put8(&s, 1); put8(&s, REAC_HEADAMP_PHANTOM); put8(&s, 2);
	put8(&s, 1); put8(&s, REAC_HEADAMP_SENS); put8(&s, 0x38);
	put8(&s, 60); put8(&s, REAC_HEADAMP_PAD); put8(&s, 1);
	put8(&s, 0); put8(&s, 8); put8(&s, 0); put8(&s, 8);
	put32(&s, 0); put16(&s, 64);
	emit("grant", "refused-cells", &s);
	/* a scene replay armed past the end of the head-amp table, the input that
	 * found its clamp wrapping onto channels 0..47 */
	static const uint8_t PAST_TABLE[24] = {
		0xff, 0xff, 0x00, 0x00, 0x00, 0x28, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9, 0xf9,
		0xf9, 0xf9, 0xf9, 0x10, 0x28, 0x10, 0x03, 0x00, 0x00, 0x00, 0xc8, 0x00,
	};
	put(&s, PAST_TABLE, sizeof PAST_TABLE);
	emit("grant", "replay-base-past-table", &s);
	/* a buffer one row too small */
	put16(&s, 0); put8(&s, 8); put8(&s, REAC_GRANT_SWEEP_LEN(8) - 1); put8(&s, 0);
	emit("grant", "buffer-short", &s);
	/* nothing set, no allocation, a replay that runs off the head-amp space */
	put16(&s, 0xffff); put8(&s, 0); put8(&s, 0); put8(&s, 0);
	put8(&s, 0x28); put8(&s, 0x10);   /* a raw allocation past the ceiling */
	put8(&s, 0x28); put8(&s, 0x10);   /* a replay from 40, 16 wide */
	put32(&s, 3); put16(&s, 200);
	emit("grant", "replay-past-ceiling", &s);
	put16(&s, 0xffff); put8(&s, 0); put8(&s, 0); put8(&s, 0);
	put8(&s, 0); put8(&s, 0);
	put8(&s, 0); put8(&s, 0);         /* no replay */
	put32(&s, 0); put16(&s, 16);
	emit("grant", "idle-scheduler", &s);
	/* the 40-wide box with a buffer of exactly its sweep, REAC_GRANT_SWEEP_LEN(40) =
	 * 128 rows, the widest sweep the u8 row count can hold */
	put16(&s, 0); put8(&s, 40); put8(&s, 128);
	put8(&s, 3);
	put8(&s, 1); put8(&s, REAC_HEADAMP_PHANTOM); put8(&s, 1);
	put8(&s, 1); put8(&s, REAC_HEADAMP_SENS); put8(&s, 0x20);
	put8(&s, 47); put8(&s, REAC_HEADAMP_PAD); put8(&s, 1);
	put8(&s, 0); put8(&s, 40); put8(&s, 0); put8(&s, 40);
	put32(&s, 64); put16(&s, 1024);
	emit("grant", "box-base0-w40-rows128", &s);
	/* a box whose one slot is 0x30, the first past the head-amp space, its cells
	 * set there; the hand-built allocation and the replay take 0x2f instead */
	put16(&s, 0x30); put8(&s, 1); put8(&s, 127);
	put8(&s, 3);
	put8(&s, 0x30); put8(&s, REAC_HEADAMP_PHANTOM); put8(&s, 1);
	put8(&s, 0x30); put8(&s, REAC_HEADAMP_SENS); put8(&s, 0x20);
	put8(&s, 47); put8(&s, REAC_HEADAMP_PAD); put8(&s, 1);
	put8(&s, 0x2f); put8(&s, 1); put8(&s, 0x2f); put8(&s, 1);
	put32(&s, 64); put16(&s, 1024);
	emit("grant", "box-base48-w1", &s);
	free(s.p);
}

/* ---- pcap files, built from fixture frames ---- */
static void pcap_header(struct seed *s, int swap)
{
	uint32_t h[6] = { 0xa1b2c3d4u, 2 | 4u << 16, 0, 0, 65535, 1 };
	for (int i = 0; i < 6; i++) {
		uint32_t v = h[i];
		if (swap)
			v = __builtin_bswap32(v);
		put32(s, v);
	}
	if (!swap)
		return;
	/* the version halves are two u16, swapped as such */
	s->p[s->n - 20] = 0x00; s->p[s->n - 19] = 0x02;
	s->p[s->n - 18] = 0x00; s->p[s->n - 17] = 0x04;
}

static void pcap_record(struct seed *s, int swap, uint32_t sec, uint32_t usec,
                        const uint8_t *p, size_t incl, size_t orig)
{
	uint32_t h[4] = { sec, usec, (uint32_t)incl, (uint32_t)orig };
	for (int i = 0; i < 4; i++)
		put32(s, swap ? __builtin_bswap32(h[i]) : h[i]);
	put(s, p, incl);
}

static void gen_pcap(void)
{
	struct seed s = { 0 };
	uint8_t tagged[REAC_FRAME_BYTES + 4];
	for (int swap = 0; swap < 2; swap++) {
		/* cap 1496: every frame fits */
		put8(&s, 187);
		pcap_header(&s, swap);
		for (size_t i = 0; i < N_FIXTURES; i++) {
			size_t n = fixture_frame(g_frame, &FIXTURES[i], (uint16_t)i);
			pcap_record(&s, swap, 1700000000u, (uint32_t)i * 250, g_frame, n, n);
		}
		emit("pcap", swap ? "fixtures-swapped" : "fixtures", &s);
	}
	/* the 0832 session as a mirror port on a trunk hands it back: VLAN 13 tagged */
	put8(&s, 187);
	pcap_header(&s, 0);
	for (size_t i = 0; i < BOX_0832_N; i++) {
		size_t n = frame_0832(g_frame, &BOX_0832_FRAMES[i]);
		memcpy(tagged, g_frame, 12);
		tagged[12] = 0x81; tagged[13] = 0x00; tagged[14] = 0x00; tagged[15] = 13;
		memcpy(tagged + 16, g_frame + 12, n - 12);
		pcap_record(&s, 0, 1758085200u, (uint32_t)i, tagged, n + 4, n + 4);
	}
	emit("pcap", "box-0832-vlan13", &s);
	/* a snaplen capture, a record bigger than the buffer, then a cut record */
	put8(&s, 44);   /* cap 352: room for an 8-wide return, not for a downstream */
	pcap_header(&s, 0);
	size_t n = program_frame(g_frame, 1);
	pcap_record(&s, 0, 1, 2, g_frame, 64, n);
	pcap_record(&s, 0, 1, 3, g_frame, n, n);
	n = fixture_frame(g_frame, &FIXTURES[8], 9);
	pcap_record(&s, 0, 1, 4, g_frame, n, n);
	pcap_record(&s, 0, 1, 5, g_frame, n, n);
	s.n -= 100;
	emit("pcap", "snaplen-oversized-truncated", &s);
	/* not a pcap at all: a pcapng section header */
	put8(&s, 187);
	put(&s, "\x0a\x0d\x0d\x0a", 4);
	put32(&s, 28);
	put32(&s, 0x1a2b3c4du);
	put16(&s, 1); put16(&s, 0);
	put32(&s, 0xffffffffu); put32(&s, 0xffffffffu);
	emit("pcap", "pcapng-refused", &s);
	/* half a global header */
	put8(&s, 187);
	pcap_header(&s, 0);
	s.n -= 10;
	emit("pcap", "half-global-header", &s);
	/* a global header and nothing else, then half a record header */
	put8(&s, 187);
	pcap_header(&s, 0);
	emit("pcap", "header-only", &s);
	put8(&s, 187);
	pcap_header(&s, 0);
	put32(&s, 1); put32(&s, 2);
	emit("pcap", "half-record-header", &s);
	/* a record too big for the buffer, cut short inside its own body */
	put8(&s, 4);
	pcap_header(&s, 0);
	n = fixture_frame(g_frame, &FIXTURES[8], 9);
	pcap_record(&s, 0, 1, 2, g_frame, n, n);
	s.n -= 10;
	emit("pcap", "oversized-cut", &s);
	/* an 8-byte buffer: a 9-byte record one byte over it, alone and then followed by
	 * a record that fits; and two equal 1-byte records */
	static const uint8_t NINE[9] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18 };
	put8(&s, 1);
	pcap_header(&s, 0);
	pcap_record(&s, 0, 1, 0, NINE, sizeof NINE, sizeof NINE);
	emit("pcap", "record-one-byte-over-cap", &s);
	put8(&s, 1);
	pcap_header(&s, 0);
	pcap_record(&s, 0, 1, 2, NINE, sizeof NINE, sizeof NINE);
	pcap_record(&s, 0, 1, 3, (const uint8_t *)"\x42", 1, 1);
	emit("pcap", "skipped-then-small", &s);
	put8(&s, 1);
	pcap_header(&s, 0);
	pcap_record(&s, 0, 1, 2, (const uint8_t *)"A", 1, 1);
	pcap_record(&s, 0, 1, 2, (const uint8_t *)"A", 1, 1);
	emit("pcap", "two-equal-records", &s);
	free(s.p);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <corpus dir>\n", argv[0]);
		return 2;
	}
	g_root = argv[1];
	mkdir_p(g_root);
	gen_decode();
	gen_ctrl();
	gen_identity();
	gen_disco();
	gen_scene();
	gen_boxreg();
	gen_knock();
	gen_grant();
	gen_pcap();
	return 0;
}
