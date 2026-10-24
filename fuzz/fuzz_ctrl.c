// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libFuzzer harness: the CONTROL PLANE parsers (src/reac_ctrlblk.c, src/reac_ctrl.c,
 * src/reac_ports.c) — what a frame is, which box sent it, and what it declared.
 *
 * Input:  [0..5]  our MAC, the address a frame must be unicast to to reach us
 *         [6]     bit 0 set: run the builder round trip below, with the catalogue
 *                 row (byte >> 1) modulo the catalogue size
 *         [7..]   frames, each a u16 LE length then its bytes, at most 256 of them
 *
 * Every frame goes through reac_ctrl_parse, the master-side box classifier, the
 * single-record identity reply, the two-fragment identity reassembly (stateful
 * across frames, as on the wire), the catalogue match and the port-table decode.
 * Each frame is its own allocation of exactly its length.
 *
 * Properties, each an abort():
 *   - reac_ctrl_parse is NONE exactly for a frame shorter than 50 B or not 0x8819,
 *     and it reads the addresses and counter where the frame carries them;
 *   - the box classifier only answers for a Roland-OUI source that is not us and,
 *     FILLER aside, only for a frame whose block checksum closes;
 *   - an identity payload points inside the frame (or the reassembly buffer) and
 *     only ever comes out of a record whose checksums close;
 *   - a decoded port table is whole 4-channel slots and never wider than its twelve;
 *   - THE BUILDERS AND THE PARSERS AGREE: what reac_ctrl_build_as emits for a
 *     catalogue row parses back as the kind it is, matches that row, declares the
 *     row's widths, and its identity records reassemble to the row's own facts.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <reac/reac.h>
#include <reac/reac_ctrl.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_identity.h>
#include <reac/reac_master.h>
#include <reac/reac_ports.h>

#include "fuzz.h"

#define MAX_FRAMES 256

static int inside(const uint8_t *p, size_t n, const uint8_t *base, size_t len)
{
	return p >= base && (size_t)(p - base) <= len && n <= len - (size_t)(p - base);
}

static void check_ports(const uint8_t *block)
{
	struct reac_box_ports ports = { -7, -7, -7 };
	int r = reac_ports_parse(block, &ports);
	uint8_t first = 0xee;
	int unk = reac_ports_unknown(block, &first);
	if (reac_ports_unknown(block, NULL) != unk)
		abort();
	/* Both doors refuse the same blocks: a complete link-1 declaration or nothing. */
	if ((r == 0) != (unk >= 0))
		abort();
	if (r != 0) {
		if (ports.in_ch != -7 || ports.out_ch != -7 || ports.headamp_base != -7)
			abort();   /* "leaving out untouched" */
		return;
	}
	if (ports.in_ch % REAC_PORTS_CH_PER_SLOT || ports.out_ch % REAC_PORTS_CH_PER_SLOT ||
	    unk % REAC_PORTS_CH_PER_SLOT)
		abort();
	/* first_code names the first slot nobody has captured, and only when there is one */
	if (unk == 0 ? first != 0 : (first == REAC_PORT_SLOT_IN || first == REAC_PORT_SLOT_IN_SPLIT ||
	                            first == REAC_PORT_SLOT_OUT || first == REAC_PORT_SLOT_EMPTY))
		abort();
	int empty = 0;
	for (int i = 0; i < REAC_PORTS_TABLE_SLOTS; i++)
		empty += block[REAC_PORTS_TABLE_OFF + i] == REAC_PORT_SLOT_EMPTY;
	/* Every one of the twelve slots is an input, an output, empty or unknown. */
	if (ports.in_ch + ports.out_ch + unk + empty * REAC_PORTS_CH_PER_SLOT !=
	    REAC_PORTS_TABLE_SLOTS * REAC_PORTS_CH_PER_SLOT)
		abort();
	if (ports.headamp_base != block[REAC_HEADAMP_BASE_OFF] * REAC_HEADAMP_BASE_MULTIPLIER)
		abort();
}

static void fuzz_frame(const uint8_t *frame, size_t len, const uint8_t our[6],
                       struct reac_identity_frag *frag, struct reac_identity *id)
{
	struct reac_ctrl_parsed p;
	enum reac_ctrl_kind k = reac_ctrl_parse(frame, len, &p);
	int is_8819 = len >= 50 && frame[12] == 0x88 && frame[13] == 0x19;
	if ((k == REAC_CTRL_NONE) == is_8819)
		abort();
	if (reac_ctrl_parse(frame, len, NULL) != k)   /* NULL out is allowed, same verdict */
		abort();
	if (k != p.kind || !reac_ctrl_kind_name(k) || strcmp(reac_ctrl_kind_name(k), "?") == 0)
		abort();
	if (is_8819) {
		if (memcmp(p.dst, frame, 6) || memcmp(p.src, frame + 6, 6) ||
		    p.counter != (uint16_t)(frame[14] | frame[15] << 8))
			abort();
		int bc = reac_ctrl_checksum_verify(frame);
		if (bc != 0 && bc != -1)
			abort();
		/* A head-amp record is only ever a link-4 SINGLE, and the record check
		 * refuses anything else whatever its bytes sum to. */
		if (k == REAC_CTRL_HEADAMP && (p.link != REAC_LINK_RECORD || p.seg != REAC_SEG_SINGLE))
			abort();
		int v = reac_ctrl_headamp_record_verify(frame);
		if (v != 0 && v != -1)
			abort();
		if (v == 0 && (p.link != REAC_LINK_RECORD || p.seg != REAC_SEG_SINGLE))
			abort();
		check_ports(frame + REAC_CTRL_BLOCK_OFF);
	}

	struct reac_ctrl_parsed q;
	enum reac_master_rx_event ev = (enum reac_master_rx_event)-1;
	if (reac_ctrl_classify_box_frame(frame, len, our, &q, &ev) == 0) {
		if (q.src[0] != 0x00 || q.src[1] != 0x40 || q.src[2] != 0xab ||
		    memcmp(q.src, our, 6) == 0)
			abort();
		if (q.kind != REAC_CTRL_FILLER && reac_ctrl_checksum_verify(frame) != 0)
			abort();
		if ((int)ev < REAC_M_RX_BOX_BCAST_FILLER || ev > REAC_M_RX_BOX_CONFIG)
			abort();
	}

	uint16_t addr = 0;
	const uint8_t *pl = NULL;
	size_t pll = 0;
	int r = reac_ctrl_identity_reply(frame, len, &addr, &pl, &pll);
	if (r < 0)
		abort();   /* no NULL argument was passed */
	if (r == 1) {
		if (!inside(pl, pll, frame, len) || reac_ctrl_checksum_verify(frame) != 0)
			abort();
		if (reac_identity_ingest(id, addr, pl, pll) < 0)
			abort();
	}

	r = reac_ctrl_identity_fragment(frag, frame, len, &addr, &pl, &pll);
	if (r < 0)
		abort();
	if (r == 1) {
		if (!inside(pl, pll, frag->buf, sizeof frag->buf))
			abort();
		if (reac_identity_ingest(id, addr, pl, pll) < 0)
			abort();
	}

	const struct reac_box_model *m = reac_box_catalogue_match(frame, len);
	if (m && k != REAC_CTRL_CONFIG_ANNOUNCE)
		abort();   /* only a declaration names a row */
}

/* Build a row's blocks and read them back through the parsers. */
static void round_trip(const struct reac_box_model *m, const uint8_t our[6], uint16_t counter)
{
	static const uint8_t box[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };
	uint8_t out[REAC_FRAME_BYTES];
	struct reac_identity id;
	struct reac_identity_frag frag;
	reac_identity_init(&id);
	memset(&frag, 0, sizeof frag);

	for (int b = REAC_BOX_BLOCK_CONFIG; b <= REAC_BOX_BLOCK_IDENT_LAST; b++) {
		size_t n = reac_ctrl_build_as(out, m, (enum reac_box_block)b, our, box, counter,
		                              NULL, 0);
		if (n == 0)
			continue;
		if (n > sizeof out || reac_frame_channels(n) == 0)
			abort();   /* a builder only emits a legal geometry */
		uint8_t *frame = fuzz_dup(out, n);
		struct reac_ctrl_parsed p;
		enum reac_ctrl_kind k = reac_ctrl_parse(frame, n, &p);
		uint16_t addr;
		const uint8_t *pl;
		size_t pll;
		switch (b) {
		case REAC_BOX_BLOCK_CONFIG: {
			struct reac_box_ports ports;
			if (k != REAC_CTRL_CONFIG_ANNOUNCE ||
			    reac_ctrl_checksum_verify(frame) != 0 ||
			    reac_box_catalogue_match(frame, n) == NULL ||
			    reac_ports_parse(frame + REAC_CTRL_BLOCK_OFF, &ports) != 0 ||
			    ports.in_ch != m->in_ch || ports.out_ch != m->out_ch)
				abort();
			break;
		}
		case REAC_BOX_BLOCK_CC0016:
		case REAC_BOX_BLOCK_CC001A:
			if (k != REAC_CTRL_GRANT ||
			    reac_ctrl_identity_reply(frame, n, &addr, &pl, &pll) != 1 ||
			    reac_identity_ingest(&id, addr, pl, pll) != 1)
				abort();
			break;
		case REAC_BOX_BLOCK_IDENT_FIRST:
			if (k != REAC_CTRL_RECORD_FRAGMENT ||
			    reac_ctrl_identity_fragment(&frag, frame, n, &addr, &pl, &pll) != 0)
				abort();
			break;
		case REAC_BOX_BLOCK_IDENT_LAST:
			if (k != REAC_CTRL_RECORD_FRAGMENT ||
			    reac_ctrl_identity_fragment(&frag, frame, n, &addr, &pl, &pll) != 1 ||
			    reac_identity_ingest(&id, addr, pl, pll) != 1)
				abort();
			break;
		default:
			if (k != REAC_CTRL_GRANT || reac_ctrl_checksum_verify(frame) != 0)
				abort();
			break;
		}
		free(frame);
	}
	/* The identity page the row emitted is the row's own facts. */
	if (!id.has_fw || id.fw_milli != m->fw_milli || !id.has_reac_version ||
	    id.reac_version_major != m->reac_major || id.reac_version_minor != m->reac_minor ||
	    id.reac_version_patch != m->reac_patch)
		abort();
	if (m->has_identity_record &&
	    (!id.has_model_name || !m->name || strcmp(id.model_name, m->name) != 0))
		abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size >= FUZZ_INPUT_MAX)
		return 0;
	struct fuzz_in in = fuzz_in(data, size);
	uint8_t our[6];
	for (int i = 0; i < 6; i++)
		our[i] = fuzz_u8(&in);
	uint8_t mode = fuzz_u8(&in);

	if (mode & 1) {
		size_t count = 0;
		const struct reac_box_model *cat = reac_box_catalogue(&count);
		if (!cat || count == 0)
			abort();
		round_trip(&cat[(size_t)(mode >> 1) % count], our, 0x1234);
	}

	struct reac_identity id;
	struct reac_identity_frag frag;
	reac_identity_init(&id);
	memset(&frag, 0, sizeof frag);

	/* The documented refusals of a bad argument, which no frame can reach. */
	{
		uint16_t a;
		const uint8_t *pl;
		size_t n;
		uint8_t code = 0xee, blk[32] = { 0 };
		if (reac_ctrl_identity_reply(NULL, 0, &a, &pl, &n) != -1 ||
		    reac_ctrl_identity_fragment(&frag, NULL, 0, &a, &pl, &n) != -1 ||
		    reac_ports_parse(blk, NULL) != -1 || reac_ports_parse(NULL, NULL) != -1 ||
		    reac_ports_unknown(NULL, &code) != -1 || code != 0 ||
		    strcmp(reac_ctrl_kind_name((enum reac_ctrl_kind)(REAC_CTRL_UNKNOWN_CTRL + 1)),
		           "?") != 0)
			abort();
	}

	for (int i = 0; i < MAX_FRAMES && in.n; i++) {
		const uint8_t *body;
		size_t len = fuzz_record(&in, &body);
		uint8_t *frame = fuzz_dup(body, len);
		fuzz_frame(frame, len, our, &frag, &id);
		free(frame);
	}
	/* Whatever was ingested is a well-formed string. */
	if (id.has_model_name && memchr(id.model_name, 0, sizeof id.model_name) == NULL)
		abort();
	return 0;
}
