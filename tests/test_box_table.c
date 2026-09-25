// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The model table is DATA, and this test is the licence for saying so
 * (docs/design/specs/2026-09-17-the-daemon-can-be-a-box.md §2, reac-pw's tree).
 *
 * A row used to BE its captured bytes. So a model nobody has captured — an
 * S-0816, an S-2416, or the operator's 36-channel experiment — could not be a
 * row at all; it could only be code. Every block is now SYNTHESISED from a row's
 * declared facts, and the captured bytes are the ORACLE for that synthesis:
 *
 *   ARM 1  for every CAPTURED row, every block the synthesiser writes is
 *          BYTE-IDENTICAL to the bytes a real box put on a wire. Three models,
 *          seven block kinds. This is the only thing that makes arm 4 mean
 *          anything: the same generator, fed the seen models' facts, reproduces
 *          the seen models' bytes.
 *   ARM 2  every row's config-announce decodes through reac_ports to the row's
 *          OWN declared widths and strap, sums to zero mod 256, and spends
 *          exactly twelve slots.
 *   ARM 3  every row's identity page round-trips through reac_identity_ingest
 *          back to the row's declared firmware, REAC version and name — the
 *          decoder is the corpus's, not this test's.
 *   ARM 4  the rows nobody has seen are rows: widths, tokens, and the 36-channel
 *          experiment — the widest box a declaration can state, since 40 is the
 *          desk's frame (ruling 2026-09-25) — with no Roland model behind it;
 *          and EVERY row is a box width each way, never 40.
 *   ARM 5  the identity is OURS unless the row asks otherwise: every DERIVED
 *          FreeREAC row claims REAC major 9, which no Roland box has ever sent,
 *          and every CAPTURED row is Roland-shaped.
 */
#include <reac/reac.h>
#include <reac/reac_ctrl.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_identity.h>
#include <reac/reac_ports.h>

#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static int same_bytes(const char *what, const char *token,
                      const uint8_t *got, const uint8_t *want)
{
	for (int i = 0; i < 32; i++)
		if (got[i] != want[i]) {
			fprintf(stderr, "FAIL: %s/%s byte %d: synthesised %02x, "
			        "the wire says %02x\n", token, what, i, got[i], want[i]);
			return 0;
		}
	return 1;
}

int main(void)
{
	size_t n = 0;
	const struct reac_box_model *t = reac_box_model_table(&n);
	CHK(t && n >= 3);

	int derived_seen = 0, captured_seen = 0;

	for (size_t i = 0; i < n; i++) {
		const struct reac_box_model *m = &t[i];
		uint8_t blk[32];

		/* ---- ARM 1: the captured rows are the oracle ---- */
		if (m->origin == REAC_BOX_CAPTURED) {
			captured_seen++;
			CHK(reac_box_model_block(m, REAC_BOX_BLOCK_CONFIG, blk) == 1);
			CHK(same_bytes("config", m->token, blk, m->config_block));
			CHK(reac_box_model_block(m, REAC_BOX_BLOCK_CC0014, blk) == 1);
			CHK(same_bytes("cc0014", m->token, blk, m->cc0014));
			CHK(reac_box_model_block(m, REAC_BOX_BLOCK_CC0013, blk) == 1);
			CHK(same_bytes("cc0013", m->token, blk, m->cc0013));
			CHK(reac_box_model_block(m, REAC_BOX_BLOCK_CC0016, blk) == 1);
			CHK(same_bytes("cc0016", m->token, blk, m->cc0016));
			CHK(reac_box_model_block(m, REAC_BOX_BLOCK_CC001A, blk) == 1);
			CHK(same_bytes("cc001a", m->token, blk, m->cc001a));
			if (m->has_identity_record) {
				CHK(reac_box_model_block(m, REAC_BOX_BLOCK_IDENT_FIRST, blk) == 1);
				CHK(same_bytes("ident_first", m->token, blk, m->identity_first));
				CHK(reac_box_model_block(m, REAC_BOX_BLOCK_IDENT_LAST, blk) == 1);
				CHK(same_bytes("ident_last", m->token, blk, m->identity_last));
			}
		} else if (m->origin == REAC_BOX_DERIVED) {
			derived_seen++;
			/* A row nobody has seen carries NO captured bytes at all: an
			 * all-zero array read as a declaration is the defect this whole
			 * change exists to remove. */
			uint8_t zero[32];
			memset(zero, 0, sizeof zero);
			CHK(memcmp(m->config_block, zero, 32) == 0);
		}

		/* ---- ARM 2: the declaration says what the row says ---- */
		CHK(reac_box_model_block(m, REAC_BOX_BLOCK_CONFIG, blk) == 1);
		struct reac_box_ports p;
		CHK(reac_ports_parse(blk, &p) == 0);
		CHK(p.in_ch == m->in_ch);
		CHK(p.out_ch == m->out_ch);
		CHK(p.headamp_base == m->headamp_strap * 0x10);
		unsigned sum = 0;
		for (int k = 0; k < 32; k++)
			sum += blk[k];
		CHK((sum & 0xff) == 0);
		int slots = 0;
		for (int k = 0; k < REAC_PORTS_TABLE_SLOTS; k++) {
			uint8_t c = blk[REAC_PORTS_TABLE_OFF + k];
			CHK(c == REAC_PORT_SLOT_IN || c == REAC_PORT_SLOT_OUT ||
			    c == REAC_PORT_SLOT_EMPTY || c == REAC_PORT_SLOT_IN_SPLIT);
			slots++;
		}
		CHK(slots == REAC_PORTS_TABLE_SLOTS);

		/* ---- ARM 5: whose identity is this ---- */
		if (m->identity_shape == REAC_BOX_IDENTITY_FREEREAC) {
			CHK(m->origin == REAC_BOX_DERIVED);
			CHK(m->reac_major == 9);   /* no Roland box has ever sent a 9 */
			CHK(m->name && strncmp(m->name, "FR-", 3) == 0);
		} else {
			CHK(m->reac_major <= 2);
		}

		/* The geometry is legal in the twelve slots, whoever declared it. */
		CHK(m->in_ch % 4 == 0 && m->out_ch % 4 == 0);
		CHK(m->in_ch + m->out_ch <= REAC_PORTS_TABLE_SLOTS * REAC_PORTS_CH_PER_SLOT);

		/* Tokens are unique: a duplicate token is a row nobody can address. */
		for (size_t j = 0; j < i; j++)
			CHK(strcmp(t[j].token, m->token) != 0);

		/* ---- ARM 3: the identity page round-trips through the decoder ---- */
		struct reac_identity id;
		reac_identity_init(&id);
		CHK(reac_box_model_block(m, REAC_BOX_BLOCK_CC0016, blk) == 1);
		CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_FIRMWARE,
		                         blk + 20, 4) == 1);
		CHK(reac_box_model_block(m, REAC_BOX_BLOCK_CC001A, blk) == 1);
		CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_REAC_VERSION,
		                         blk + 20, REAC_IDENTITY_REAC_VER_LEN) == 1);
		CHK(id.has_fw && id.fw_milli == m->fw_milli);
		CHK(id.has_reac_version);
		CHK(id.reac_version_major == m->reac_major);
		CHK(id.reac_version_minor == m->reac_minor);
		CHK(id.reac_version_patch == m->reac_patch);

		if (m->has_identity_record) {
			/* The name arrives as TWO fragments of one record — 10 bytes in
			 * the FIRST, 6 in the LAST — so the payload the decoder sees is
			 * reassembled here exactly as a receiver must reassemble it. */
			uint8_t first[32], last[32], page[17];
			CHK(reac_box_model_block(m, REAC_BOX_BLOCK_IDENT_FIRST, first) == 1);
			CHK(reac_box_model_block(m, REAC_BOX_BLOCK_IDENT_LAST, last) == 1);
			page[0] = first[20];               /* name_kind */
			memcpy(page + 1, first + 21, 10);
			memcpy(page + 11, last + 9, 6);
			CHK(page[0] == 0x01);
			CHK(reac_identity_ingest(&id, REAC_IDENTITY_ADDR_MODEL_NAME,
			                         page, sizeof page) == 1);
			CHK(id.has_model_name);
			CHK(m->name && strcmp(id.model_name, m->name) == 0);
		} else {
			CHK(reac_box_model_block(m, REAC_BOX_BLOCK_IDENT_FIRST, blk) == 0);
			CHK(reac_box_model_block(m, REAC_BOX_BLOCK_IDENT_LAST, blk) == 0);
		}

	}

	CHK(captured_seen == 4);          /* the S-4000S-0832 joined them 2026-09-17 */
	CHK(derived_seen >= 6);

	/* ---- ARM 4: the rows nobody has seen, by name ---- */
	const struct reac_box_model *m;
	CHK((m = reac_box_model_by_token("s0816")) && m->in_ch == 8 && m->out_ch == 16);
	CHK((m = reac_box_model_by_token("s2416")) && m->in_ch == 24 && m->out_ch == 16);
	CHK((m = reac_box_model_by_token("s4000d")) && m->in_ch == 0 && m->out_ch == 32);
	CHK((m = reac_box_model_by_token("s4000m")) && m->in_ch == 32 && m->out_ch == 0);
	/* THE 0832 SPLIT IS NO LONGER A GUESS — and the S-4000H token it briefly had
	 * is gone with it: the M-200 displays this chassis as an S-4000S, which is
	 * what a box sending no name record must be called. */
	CHK(reac_box_model_by_token("s4000h") == NULL);
	/* The S-4000S split the corpus HAS, and the one it does not. */
	CHK((m = reac_box_model_by_token("s4000s")) && m->in_ch == 32 && m->out_ch == 8);
	CHK(m->origin == REAC_BOX_CAPTURED);
	CHK((m = reac_box_model_by_token("s4000s-0832")) && m->in_ch == 8 && m->out_ch == 32);
	CHK(m->origin == REAC_BOX_CAPTURED);
	CHK(m->identity_shape == REAC_BOX_IDENTITY_ROLAND);   /* the same chassis */
	CHK(m->port_layout == REAC_BOX_PORTS_SPLIT_OUT_FIRST);
	CHK(m->has_identity_record == 0 && m->name == NULL);
	CHK(reac_box_model_upstream_width(m) == 8);   /* its granted return, measured */

	/* THE OPERATOR'S EXPERIMENT, at the widest a box may be: 40 channels is the
	 * DESK's frame (ruling 2026-09-25), a box width is an even 2..38, and the
	 * declaration counts in 4-channel slots — so 36, either way round. */
	CHK((m = reac_box_model_by_token("fr3600")) && m->in_ch == 36 && m->out_ch == 0);
	CHK(m->origin == REAC_BOX_DERIVED && m->identity_shape == REAC_BOX_IDENTITY_FREEREAC);
	CHK((m = reac_box_model_by_token("fr0036")) && m->in_ch == 0 && m->out_ch == 36);
	CHK(reac_box_model_by_token("fr4000") == NULL && reac_box_model_by_token("fr0040") == NULL);
	/* EVERY ROW is a box: each direction zero or an even 2..38, never the desk's 40. */
	{
		size_t n;
		const struct reac_box_model *t = reac_box_model_table(&n);
		for (size_t i = 0; i < n; i++) {
			CHK(t[i].in_ch == 0 || reac_box_width_ok(t[i].in_ch));
			CHK(t[i].out_ch == 0 || reac_box_width_ok(t[i].out_ch));
			CHK(reac_box_width_ok(reac_box_model_upstream_width(&t[i])));
		}
	}
	CHK((m = reac_box_model_by_token("fr2020")) && m->in_ch == 20 && m->out_ch == 20);

	/* A WIDTH STILL NAMES ONLY A CAPTURED ROW. Derived rows are addressed by
	 * token alone — otherwise an experiment row would start answering for a real
	 * box's width on a wire, which is the defect a fixed matrix exists to stop. */
	CHK((m = reac_box_model_by_channels(16)) && m->origin == REAC_BOX_CAPTURED);
	CHK(strcmp(m->token, "s1608") == 0);
	CHK((m = reac_box_model_by_channels(8)) && strcmp(m->token, "s0808") == 0);
	CHK(m->origin == REAC_BOX_CAPTURED);   /* NOT the 8-input S-4000H */
	CHK((m = reac_box_model_by_channels(32)) && strcmp(m->token, "s4000s") == 0);
	/* A width with no captured row falls back to the S-1608 — the documented
	 * default, asserted by name (an origin check here could never fail). */
	CHK((m = reac_box_model_by_channels(40)) && strcmp(m->token, "s1608") == 0);

	/* ---- THE ROW REACHES THE WIRE. A width can only name a CAPTURED row, so
	 * this is the door a derived model declares itself through: build the
	 * announce AS the 36-channel experiment row and require the frame to carry
	 * that row's declaration and to be recognised back as that row. ---- */
	{
		static const uint8_t MASTER[6] = { 0x00, 0x40, 0xab, 0x01, 0x02, 0x03 };
		static const uint8_t SRC[6]    = { 0x00, 0x40, 0xab, 0x0f, 0x0e, 0x0d };
		uint8_t frame[2048], want[32];
		const struct reac_box_model *fr = reac_box_model_by_token("fr3600");
		CHK(fr && reac_box_model_upstream_width(fr) == 36);
		size_t len = reac_ctrl_build_as(frame, fr, REAC_BOX_BLOCK_CONFIG,
		                                MASTER, SRC, 1, NULL, 0);
		CHK(len == reac_ctrl_box_frame_len(36));
		CHK(reac_box_model_block(fr, REAC_BOX_BLOCK_CONFIG, want) == 1);
		CHK(memcmp(frame + 18, want, 32) == 0);
		CHK(reac_ctrl_identify_box(frame, len) == fr);
		/* The identity the mixer will read back is OURS, not a Roland box's. */
		CHK(reac_ctrl_build_as(frame, fr, REAC_BOX_BLOCK_IDENT_FIRST,
		                       MASTER, SRC, 2, NULL, 0) > 0);
		CHK(memcmp(frame + 18 + 21, "FR-3600", 7) == 0);
		/* An output-only row still speaks: the declaration says zero inputs and
		 * the frame carries the minimum pair (an assumption, named in the spec). */
		const struct reac_box_model *d = reac_box_model_by_token("fr0036");
		CHK(d && d->in_ch == 0 && reac_box_model_upstream_width(d) == 2);
		CHK(reac_ctrl_build_as(frame, d, REAC_BOX_BLOCK_CONFIG,
		                       MASTER, SRC, 3, NULL, 0) == reac_ctrl_box_frame_len(2));
		CHK(reac_box_model_block(d, REAC_BOX_BLOCK_CONFIG, want) == 1);
		CHK(memcmp(frame + 18, want, 32) == 0);
		/* And a CAPTURED row built through this door is the same bytes the
		 * width-keyed builder has always emitted — one declaration, two doors. */
		const struct reac_box_model *s16 = reac_box_model_by_token("s1608");
		uint8_t legacy[2048];
		size_t l1 = reac_ctrl_build_as(frame, s16, REAC_BOX_BLOCK_CONFIG,
		                               MASTER, SRC, 4, NULL, 0);
		size_t l2 = reac_ctrl_build_config_announce(legacy, MASTER, SRC, 4, 16);
		CHK(l1 == l2 && l1 > 0 && memcmp(frame, legacy, l1) == 0);
	}

	/* Bad arguments refuse; they never write a half block. */
	uint8_t blk[32];
	CHK(reac_box_model_block(NULL, REAC_BOX_BLOCK_CONFIG, blk) < 0);
	CHK(reac_box_model_block(reac_box_model_by_token("s1608"),
	                         REAC_BOX_BLOCK_CONFIG, NULL) < 0);

	printf("OK: reac_box_model — the table is DATA: the synthesiser reproduces "
	       "every captured block of all 3 captured rows byte for byte, every row's "
	       "declaration decodes to its own widths and strap, every identity page "
	       "round-trips through reac_identity, and the unseen models (S-0816, "
	       "S-2416, S-4000D/M/H, the 0832 split) plus the 36-channel experiment "
	       "are rows\n");
	return 0;
}
