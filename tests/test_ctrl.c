// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_ctrl_parse over REAL control blocks — one per distinct wire shape the
 * capture corpus carries (tests/ctrl_fixtures.inc).
 *
 * The classifier had no test at all while it decided what a frame was from its
 * LENGTH, and the corpus is the reason that survived: switching on the length
 * happens to work, because each sub-page has a distinct one. It stops working on
 * a window that is not full, and it never worked on the two shapes that share a
 * length. So every assertion below names the field the box's own dispatch reads
 * — the link at block[0], the segment bits at block[1], the opcode at block[4] —
 * and the fixtures are the corpus's own bytes, not a shape written to pass.
 *
 * THE NEGATIVE CONTROLS ARE THE POINT of half this file: two blocks that differ
 * ONLY in an opcode must classify apart, and a first fragment must refuse to be
 * read as a whole record.
 */

#include <reac/reac_ctrlblk.h>
#include <reac/reac_ports.h>
#include <reac/reac.h>
#include <string.h>
#include <stdio.h>

#include "ctrl_fixtures.inc"

#define CHK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL: %s (line %d)\n", #x, __LINE__); return 1; } } while (0)

/* Wrap a [16:50] window back into the smallest frame the parser accepts. */
static void frame_of(uint8_t f[REAC_CTRL_BLOCK_END], const uint8_t win[34])
{
	memset(f, 0, REAC_CTRL_BLOCK_END);
	f[12] = 0x88; f[13] = 0x19;
	memcpy(f + 16, win, 34);
}

static enum reac_ctrl_kind kind_of(const uint8_t win[34], struct reac_ctrl_parsed *p)
{
	uint8_t f[REAC_CTRL_BLOCK_END];
	frame_of(f, win);
	return reac_ctrl_parse(f, sizeof f, p);
}

int main(void)
{
	struct reac_ctrl_parsed p;

	/* ---- every shape the corpus holds, classified by link/segment/opcode ---- */
	struct { const uint8_t *win; enum reac_ctrl_kind kind; } EXPECT[] = {
		{ FX_FILLER,         REAC_CTRL_FILLER          },
		{ FX_ANNOUNCE,       REAC_CTRL_MASTER_ANNOUNCE },
		{ FX_L1_BULK_FIRST,  REAC_CTRL_SCENE_TRANSFER  },
		{ FX_L1_BULK_MIDDLE, REAC_CTRL_SCENE_TRANSFER  },
		{ FX_L1_BULK_LAST,   REAC_CTRL_SCENE_TRANSFER  },
		{ FX_L1_BULK_SINGLE, REAC_CTRL_SCENE_TRANSFER  },
		{ FX_L1_SLOT_MAP,    REAC_CTRL_MASTER_HB       },
		{ FX_L1_GROUP_MAP,   REAC_CTRL_GROUP_MAP       },
		{ FX_L1_BOX_HB,      REAC_CTRL_BOX_HB          },
		{ FX_L1_DECL_82,     REAC_CTRL_CONFIG_ANNOUNCE },
		{ FX_L1_DECL_84,     REAC_CTRL_CONFIG_ANNOUNCE },
		{ FX_L4_HEAD_MARK,   REAC_CTRL_GRANT           },
		{ FX_L4_JOIN_GRANT,  REAC_CTRL_GRANT           },
		{ FX_L4_HEADAMP,     REAC_CTRL_HEADAMP         },
		{ FX_L4_BOX_READY,   REAC_CTRL_GRANT           },
		{ FX_L4_IDENTITY,    REAC_CTRL_GRANT           },
		{ FX_L4_RETURN,      REAC_CTRL_GRANT           },
		{ FX_FRAG_FIRST,     REAC_CTRL_RECORD_FRAGMENT },
		{ FX_FRAG_LAST,      REAC_CTRL_RECORD_FRAGMENT },
	};
	for (size_t i = 0; i < sizeof EXPECT / sizeof EXPECT[0]; i++) {
		enum reac_ctrl_kind k = kind_of(EXPECT[i].win, &p);
		if (k != EXPECT[i].kind) {
			fprintf(stderr, "FAIL: fixture %zu classified %s, expected %s\n",
			        i, reac_ctrl_kind_name(k),
			        reac_ctrl_kind_name(EXPECT[i].kind));
			return 1;
		}
	}

	/* ---- the header is four fields, and they read back ---- */
	kind_of(FX_L1_BULK_FIRST, &p);
	CHK(p.link == REAC_LINK_CTRL && p.seg == REAC_SEG_FIRST);
	CHK(p.opcode == REAC_OP_BULK && p.blk_len == 0x0018);
	kind_of(FX_L1_BULK_MIDDLE, &p);
	CHK(p.seg == REAC_SEG_MIDDLE && p.blk_len == 0x001a);
	kind_of(FX_L1_BULK_LAST, &p);
	CHK(p.seg == REAC_SEG_LAST && p.blk_len == 0x000e);
	kind_of(FX_L1_SLOT_MAP, &p);
	CHK(p.seg == REAC_SEG_SINGLE && p.opcode == REAC_OP_SLOT_MAP);
	CHK(p.blk_len == 0x0019);            /* 25 = 1 + 8 x 3 chanmap records */

	/* THE LENGTH IS NOT THE DISCRIMINATOR. The declaration and the group map
	 * both sit on link 1 SINGLE and differ only at block[4]; the two bulk states
	 * MIDDLE and LAST differ only at block[1]. Force each pair apart. */
	kind_of(FX_L1_DECL_82, &p);
	uint16_t decl_len = p.blk_len;
	CHK(p.opcode == REAC_OP_DECL);
	kind_of(FX_L1_DECL_84, &p);
	CHK(p.opcode == REAC_OP_DECL_OTHER && p.blk_len == decl_len);
	kind_of(FX_L1_GROUP_MAP, &p);
	CHK(p.opcode == REAC_OP_GROUP_MAP && p.blk_len != decl_len);

	/* A SHORT SLOT-RECORD WINDOW IS STILL A SLOT-RECORD WINDOW. Shorten the
	 * declared length past every value the corpus carries: the opcode still says
	 * what the message is, and a length-driven classifier lands on the bulk
	 * transfer instead. This is the defect, expressed as a test. */
	uint8_t shortened[34];
	memcpy(shortened, FX_L1_SLOT_MAP, sizeof shortened);
	shortened[2 + 2] = 0x00; shortened[2 + 3] = 0x07;   /* block[2:4] = 7 */
	CHK(kind_of(shortened, &p) == REAC_CTRL_MASTER_HB);
	CHK(p.blk_len == 0x0007 && p.opcode == REAC_OP_SLOT_MAP);

	/* ---- the head-amp record, and its two nested checksums ---- */
	uint8_t f[REAC_CTRL_BLOCK_END];
	frame_of(f, FX_L4_HEADAMP);
	CHK(reac_ctrl_parse(f, sizeof f, &p) == REAC_CTRL_HEADAMP);
	CHK(p.dt1_tag == REAC_DT1_TAG_HEADAMP);
	CHK(p.param <= REAC_HEADAMP_SENS && p.ch < REAC_HEADAMP_MAX_CH);
	CHK(reac_ctrl_headamp_record_verify(f) == 0);
	CHK(reac_ctrl_checksum_verify(f) == 0);
	f[34 + 2] ^= 0xff;                                  /* corrupt the record */
	CHK(reac_ctrl_headamp_record_verify(f) == -1);

	/* A FRAGMENT IS NOT A RECORD. The first fragment carries the DT1 preamble
	 * and TAG, so every offset the head-amp verifier reads is populated and the
	 * sum comes out wrong for a reason that means nothing. Refuse it by segment
	 * instead of returning a failed checksum. */
	frame_of(f, FX_FRAG_FIRST);
	CHK(reac_ctrl_parse(f, sizeof f, &p) == REAC_CTRL_RECORD_FRAGMENT);
	CHK(p.link == REAC_LINK_RECORD && p.seg == REAC_SEG_FIRST);
	CHK(p.dt1_tag == 0);                       /* nothing is read out of half */
	CHK(reac_ctrl_headamp_record_verify(f) == -1);

	/* AND THE CHECKSUM CANNOT BE WHAT REFUSES IT. The real fragment's first six
	 * record bytes sum to 0x69, so a verifier with no segment test rejects it
	 * for the right answer by luck. Add 0x17 to one payload byte and the six
	 * bytes sum to 0x80 — a first fragment that passes the record checksum. Only
	 * block[1] tells it from a whole record, which is the negative control for
	 * the guard above: delete the guard and this assertion goes green. */
	uint8_t forged[34];
	memcpy(forged, FX_FRAG_FIRST, sizeof forged);
	unsigned six = 0;
	for (int i = 18; i < 24; i++)
		six += forged[i];
	CHK(six % 256 != 0x80);
	forged[23] = (uint8_t)(forged[23] + (0x80 - six % 256));
	frame_of(f, forged);
	reac_ctrl_checksum_apply(f);         /* valid in every respect but the field
	                                      * under test - an outer checksum left
	                                      * wrong would give the guard a second
	                                      * reason to refuse and hide whether it
	                                      * is the one doing the work */
	CHK(reac_ctrl_checksum_verify(f) == 0);
	CHK(reac_ctrl_record_cksum_verify(f + 34, 6) == 0);   /* it does sum to 0x80 */
	/* AND THE PARSER MUST REACH THE GUARD. Without this the fixture could stop
	 * being a link-4 fragment at all and the assertion below would pass for the
	 * wrong reason - a guard nothing feeds is decoration. */
	CHK(reac_ctrl_parse(f, sizeof f, &p) == REAC_CTRL_RECORD_FRAGMENT);
	CHK(p.link == REAC_LINK_RECORD && p.seg == REAC_SEG_FIRST);
	CHK(reac_ctrl_headamp_record_verify(f) == -1);        /* and is still half   */

	/* THE SPLIT RECORD'S CHECKSUM CLOSES ONLY ACROSS BOTH FRAGMENTS. Sum the
	 * SysEx address and data bytes of the pair: from the TAG in the first
	 * fragment (block[16], i.e. window[18]) through the byte before the
	 * checksum in the second. */
	unsigned sum = 0;
	/* window[18] is block[16], the TAG; the payload runs to block[30] because
	 * block[31] is the OUTER checksum and belongs to the frame, not the record. */
	for (int i = 18; i < 33; i++)
		sum += FX_FRAG_FIRST[i];
	int cks_at = -1;
	for (int i = 2 + 9; i < 34; i++)               /* second: payload to the f7 */
		if (FX_FRAG_LAST[i] == 0xf7) { cks_at = i - 1; break; }
	CHK(cks_at > 2 + 9);
	for (int i = 2 + 9; i < cks_at; i++)
		sum += FX_FRAG_LAST[i];
	CHK(sum == 358);
	CHK(FX_FRAG_LAST[cks_at] == (128 - sum % 128));
	CHK(FX_FRAG_LAST[cks_at] == 0x1a);

	/* Neither fragment closes on its own — the negative control for the above. */
	unsigned first_only = 0;
	for (int i = 18; i < 33; i++)
		first_only += FX_FRAG_FIRST[i];
	CHK(first_only % 128 != 0);

	/* ---- the box's declaration reaches the model matrix and the port table -- */
	frame_of(f, FX_L1_DECL_82);
	CHK(reac_ctrl_identify_box(f, sizeof f) != NULL);
	struct reac_box_ports ports;
	CHK(reac_ports_parse(f + REAC_CTRL_BLOCK_OFF, &ports) == 0);
	CHK(ports.in_ch == 16 && ports.out_ch == 8);
	frame_of(f, FX_L1_DECL_84);
	CHK(reac_ports_parse(f + REAC_CTRL_BLOCK_OFF, &ports) == 0);
	CHK(ports.in_ch == 8 && ports.out_ch == 8);
	/* and no other link-1 message does */
	frame_of(f, FX_L1_SLOT_MAP);
	CHK(reac_ctrl_identify_box(f, sizeof f) == NULL);
	CHK(reac_ports_parse(f + REAC_CTRL_BLOCK_OFF, &ports) == -1);
	frame_of(f, FX_L1_GROUP_MAP);
	CHK(reac_ports_parse(f + REAC_CTRL_BLOCK_OFF, &ports) == -1);

	/* ---- THE BUILDERS EMIT THE PAIR, AND THE PAIR CLOSES ------------------
	 * A model either sends the identity record or it does not; it never sends
	 * half. The two builders read one matrix flag, so the emitted fragments
	 * cannot get out of step, and the record they carry is the one the corpus
	 * holds - byte for byte, including the 0x1a the second frame closes with. */
	static const uint8_t MASTER[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
	static const uint8_t SRC[6]    = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x02 };
	uint8_t a[REAC_FRAME_BYTES], b[REAC_FRAME_BYTES];
	size_t na = reac_ctrl_build_identity_first(a, MASTER, SRC, 1, 8);
	size_t nb = reac_ctrl_build_identity_last(b, MASTER, SRC, 2, 8);
	CHK(na > 0 && nb > 0);                     /* the S-0808 sends the record */
	CHK(memcmp(a + 16, FX_FRAG_FIRST, 34) == 0);
	CHK(memcmp(b + 16, FX_FRAG_LAST, 34) == 0);
	CHK(reac_ctrl_parse(a, na, &p) == REAC_CTRL_RECORD_FRAGMENT && p.seg == REAC_SEG_FIRST);
	CHK(reac_ctrl_parse(b, nb, &p) == REAC_CTRL_RECORD_FRAGMENT && p.seg == REAC_SEG_LAST);

	/* ONE FLAG GATES BOTH, so a model cannot be made to emit a first fragment
	 * with nothing to close it. Both widths that send no record send neither. */
	CHK(reac_ctrl_build_identity_first(a, MASTER, SRC, 1, 16) == 0);
	CHK(reac_ctrl_build_identity_last(b, MASTER, SRC, 2, 16) == 0);
	CHK(reac_ctrl_build_identity_first(a, MASTER, SRC, 1, 32) == 0);
	CHK(reac_ctrl_build_identity_last(b, MASTER, SRC, 2, 32) == 0);

	/* ---- NULL out is allowed, as the header says ---- */
	frame_of(f, FX_L1_BOX_HB);
	CHK(reac_ctrl_parse(f, sizeof f, NULL) == REAC_CTRL_BOX_HB);

	/* ---- not a REAC frame ---- */
	memset(f, 0, sizeof f);
	CHK(reac_ctrl_parse(f, sizeof f, &p) == REAC_CTRL_NONE);
	frame_of(f, FX_L1_BOX_HB);
	CHK(reac_ctrl_parse(f, REAC_CTRL_BLOCK_END - 1, &p) == REAC_CTRL_NONE);

	/* 0.7.2: the two frames a box sends a stagebox on M, against the bytes a real
	 * S-1608 sent a real S-0808 on 2026-09-09 (box-to-box-enroll.pcap), four
	 * milliseconds before it was granted. The middle burst record is GENERATED — the
	 * DT1 container with tag 0x0000, data 03 00 00 00 and the ordinary Roland record
	 * checksum, which computes to the 0x7d that box put on the wire. */
	{
		static const uint8_t M[6] = { 0x00, 0x40, 0xab, 0xc4, 0xdc, 0x9c };
		static const uint8_t S[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x41 };
		uint8_t f[2048];
		char got[80];
		size_t n = reac_ctrl_build_coldconnect_head(f, M, S, 0, 8, NULL, 0);
		CHK(n == 340);
		for (int i = 0; i < 34; i++)
			sprintf(got + i * 2, "%02x", f[16 + i]);
		CHK(strcmp(got, "cdea04030014000200fe0ff0410a0000"
		                  "12120000030000007df70000000000000000") == 0);
		CHK(reac_ctrl_record_cksum_verify(f + REAC_CTRL_BLOCK_OFF + 16, 7) == 0);
		CHK(reac_ctrl_checksum_verify(f) == 0);
		/* and it is NOT the JOIN record: a burst that repeats one draws one fewer
		 * echo from the master, measured by replay. */
		uint8_t g[2048];
		CHK(reac_ctrl_build_coldconnect(g, M, S, 0, 8, NULL, 0) == 340);
		CHK(memcmp(f + 16, g + 16, 34) != 0);

		n = reac_ctrl_build_config_announce_box_master(f, M, S, 0, 8);
		CHK(n == 340);
		for (int i = 0; i < 34; i++)
			sprintf(got + i * 2, "%02x", f[16 + i]);
		CHK(strcmp(got, "cdea010300108000000002020202"
		                  "0101030303030303000000000000000000000050") == 0);
		CHK(reac_ctrl_checksum_verify(f) == 0);
	}

	printf("OK: reac_ctrl_parse over every control shape the corpus carries — "
	       "link/segment/opcode, not length; a split record's checksum closes "
	       "only across both fragments\n");
	return 0;
}
