// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_box_synth — a box model's wire blocks, SYNTHESISED from the row's
 * declared facts (reac_ctrlblk.h's struct reac_box_model; the ruling is
 * docs/design/specs/2026-09-17-the-daemon-can-be-a-box.md §2, in reac-pw's tree).
 *
 * WHY THIS FILE EXISTS. A row used to BE its captured bytes, so a model nobody
 * has captured could not be a row — only code. Here a row is its FACTS and the
 * bytes are derived, by the grammar the corpus already pinned: reac_ports.h's
 * twelve-slot port table, reac_identity.h's DT1 identity page, and the two
 * checksums reac_ctrlblk.c already stamps in that order. The captured rows keep
 * their bytes as the ORACLE — tests/test_box_table.c requires this generator to
 * reproduce every one of them byte for byte, and that equality is the whole
 * licence for emulating a model nobody has ever seen.
 *
 * KERNEL-PORTABLE, like reac_identity and reac_ctrlblk: no allocation, no
 * floating point, caller-owned buffers, failure as a return value. */

#include <reac/reac.h>          /* REAC_MAX_CHANNELS */
#include <reac/reac_ctrlblk.h>
#include <reac/reac_identity.h>
#include <reac/reac_ports.h>

#include <string.h>

#define BLK 32

/* The REAC control block's OUTER check byte: the 32 bytes sum to 0 mod 256.
 * Stamped LAST, because it covers the DT1 inner checksum. */
static void block_check(uint8_t *b)
{
	unsigned s = 0;
	for (int i = 0; i < BLK - 1; i++)
		s += b[i];
	b[BLK - 1] = (uint8_t)((256u - (s & 0xffu)) & 0xffu);
}

/* The Roland DT1 INNER checksum: the address and data bytes sum to 0 mod 128. */
static uint8_t dt1_check(const uint8_t *p, size_t n)
{
	unsigned s = 0;
	for (size_t i = 0; i < n; i++)
		s += p[i];
	return (uint8_t)((128u - (s & 0x7fu)) & 0x7fu);
}

/* The link-4 record scaffold every identity-page block shares: the REAC header,
 * the two length echoes, and the Roland DT1 preamble up to the 4-byte address.
 * `sysex_len` is what block[8] carries — the bytes from 0xf0 to 0xf7 inclusive —
 * and block[2:4] is that plus five, which is what every captured record does. */
static void dt1_head(uint8_t *out, uint8_t frag, unsigned sysex_len,
                     uint16_t addr_lo)
{
	unsigned hdr = sysex_len + 5u;
	out[0] = 0x04;                       /* link 4                         */
	out[1] = frag;                       /* 01 FIRST, 02 LAST, 03 SINGLE   */
	out[2] = (uint8_t)(hdr >> 8);
	out[3] = (uint8_t)(hdr & 0xff);
	out[4] = 0x00; out[5] = 0x02; out[6] = 0x00; out[7] = 0xfe;
	out[8] = (uint8_t)sysex_len;
	out[9] = 0xf0;                       /* SysEx start                    */
	out[10] = 0x41;                      /* Roland                         */
	out[11] = 0x0a;                      /* device                         */
	out[12] = 0x00; out[13] = 0x00;
	out[14] = 0x12; out[15] = 0x12;      /* model id + DT1 command         */
	out[16] = (uint8_t)(REAC_IDENTITY_TAG >> 8);
	out[17] = (uint8_t)(REAC_IDENTITY_TAG & 0xff);
	out[18] = (uint8_t)(addr_lo >> 8);
	out[19] = (uint8_t)(addr_lo & 0xff);
}

/* THE TWO COLD-CONNECT JOIN RECORDS ARE THE TABLE'S CONSTANTS, NOT A ROW'S.
 * Every model in the corpus sends these two byte-identical — the S-1608, the
 * S-0808 and the S-4000S all do — so they are declared once here rather than
 * copied into each row, where three identical copies would read as three
 * independent facts. Captured: matrix-m200-s1608 / -s0808 2026-07-11. */
static const uint8_t JOIN_0014[BLK] = {
	0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe,
	0x0f, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
	0x01, 0x00, 0x06, 0x00, 0x01, 0x00, 0x78, 0xf7,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t JOIN_0013[BLK] = {
	0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe,
	0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
	0x03, 0x02, 0x00, 0x01, 0x00, 0x7a, 0xf7, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 };

/* The config-announce: the declaration a mixer reads the box's geometry out of.
 * Refuses a geometry the twelve slots cannot hold rather than truncating it — a
 * silently clamped width reaches the wire as a patch on the wrong channels. */
static int build_config(const struct reac_box_model *m, uint8_t *out)
{
	if (m->in_ch < 0 || m->out_ch < 0)
		return -1;
	if (m->in_ch % REAC_PORTS_CH_PER_SLOT || m->out_ch % REAC_PORTS_CH_PER_SLOT)
		return -1;
	int in_slots = m->in_ch / REAC_PORTS_CH_PER_SLOT;
	int out_slots = m->out_ch / REAC_PORTS_CH_PER_SLOT;
	if (in_slots + out_slots > REAC_PORTS_TABLE_SLOTS)
		return -1;

	out[0] = 0x01; out[1] = 0x03; out[2] = 0x00; out[3] = 0x10;
	out[4] = m->selector;
	out[5] = 0x00; out[6] = 0x00;
	out[7] = m->headamp_strap;      /* the chassis strap; base = strap * 0x10 */
	int k = 0;
	for (int i = 0; i < in_slots; i++)
		out[REAC_PORTS_TABLE_OFF + k++] = REAC_PORT_SLOT_IN;
	for (int i = 0; i < out_slots; i++)
		out[REAC_PORTS_TABLE_OFF + k++] = REAC_PORT_SLOT_OUT;
	while (k < REAC_PORTS_TABLE_SLOTS)
		out[REAC_PORTS_TABLE_OFF + k++] = REAC_PORT_SLOT_EMPTY;
	memcpy(out + REAC_PORTS_TABLE_OFF + REAC_PORTS_TABLE_SLOTS, m->tail,
	       sizeof m->tail);
	block_check(out);
	return 1;
}

/* One complete DT1 SINGLE record: address, data, inner checksum, f7. */
static int build_dt1_single(uint8_t *out, uint16_t addr_lo,
                            const uint8_t *data, unsigned dlen)
{
	dt1_head(out, 0x03, 7u + 4u + dlen + 2u, addr_lo);
	memcpy(out + 20, data, dlen);
	out[20 + dlen] = dt1_check(out + 16, 4u + dlen);
	out[21 + dlen] = 0xf7;
	block_check(out);
	return 1;
}

/* addr 0x0000 — the firmware version, four bytes of ONE DECIMAL DIGIT each. */
static int build_firmware(const struct reac_box_model *m, uint8_t *out)
{
	uint16_t v = m->fw_milli;
	uint8_t d[4];
	if (v > 9999)
		return -1;
	d[0] = (uint8_t)(v / 1000u);
	d[1] = (uint8_t)((v / 100u) % 10u);
	d[2] = (uint8_t)((v / 10u) % 10u);
	d[3] = (uint8_t)(v % 10u);
	return build_dt1_single(out, REAC_IDENTITY_ADDR_FIRMWARE, d, 4);
}

/* addr 0x0600 — the REAC version: four u16be, a reserved word then the triple. */
static int build_reac_version(const struct reac_box_model *m, uint8_t *out)
{
	uint8_t d[REAC_IDENTITY_REAC_VER_LEN];
	d[0] = 0x00; d[1] = 0x00;
	d[2] = (uint8_t)(m->reac_major >> 8); d[3] = (uint8_t)(m->reac_major & 0xff);
	d[4] = (uint8_t)(m->reac_minor >> 8); d[5] = (uint8_t)(m->reac_minor & 0xff);
	d[6] = (uint8_t)(m->reac_patch >> 8); d[7] = (uint8_t)(m->reac_patch & 0xff);
	return build_dt1_single(out, REAC_IDENTITY_ADDR_REAC_VERSION, d,
	                        REAC_IDENTITY_REAC_VER_LEN);
}

/* addr 0x1000 — the model NAME: one name_kind byte and sixteen NUL-padded ASCII
 * bytes, which do not fit one 32-byte block. The record is therefore TWO frames,
 * 10 name bytes in the FIRST and 6 in the LAST, and its inner checksum closes
 * only across both — which is why one flag gates both and they cannot get out of
 * step (reac_ctrlblk.h's struct comment). */
#define NAME_KIND     0x01
#define NAME_LEN      16
#define NAME_IN_FIRST 10

static void name_field(const struct reac_box_model *m, uint8_t nm[NAME_LEN])
{
	memset(nm, 0, NAME_LEN);
	if (!m->name)
		return;
	for (int i = 0; i < NAME_LEN && m->name[i]; i++)
		nm[i] = (uint8_t)m->name[i];
}

static int build_ident_first(const struct reac_box_model *m, uint8_t *out)
{
	uint8_t nm[NAME_LEN];
	name_field(m, nm);
	dt1_head(out, 0x01, 7u + 4u + 1u + NAME_IN_FIRST,
	         REAC_IDENTITY_ADDR_MODEL_NAME);
	out[20] = NAME_KIND;
	memcpy(out + 21, nm, NAME_IN_FIRST);
	block_check(out);
	return 1;
}

static int build_ident_last(const struct reac_box_model *m, uint8_t *out)
{
	uint8_t nm[NAME_LEN], rec[4 + 1 + NAME_LEN];
	name_field(m, nm);
	/* The checksum spans the WHOLE record — address, kind and all sixteen name
	 * bytes — not just this fragment's share of it. */
	rec[0] = (uint8_t)(REAC_IDENTITY_TAG >> 8);
	rec[1] = (uint8_t)(REAC_IDENTITY_TAG & 0xff);
	rec[2] = (uint8_t)(REAC_IDENTITY_ADDR_MODEL_NAME >> 8);
	rec[3] = (uint8_t)(REAC_IDENTITY_ADDR_MODEL_NAME & 0xff);
	rec[4] = NAME_KIND;
	memcpy(rec + 5, nm, NAME_LEN);

	out[0] = 0x04; out[1] = 0x02;
	out[2] = 0x00; out[3] = (uint8_t)((NAME_LEN - NAME_IN_FIRST) + 2u + 5u);
	out[4] = 0x00; out[5] = 0x02; out[6] = 0x00; out[7] = 0xfe;
	out[8] = (uint8_t)((NAME_LEN - NAME_IN_FIRST) + 2u);
	memcpy(out + 9, nm + NAME_IN_FIRST, NAME_LEN - NAME_IN_FIRST);
	out[15] = dt1_check(rec, sizeof rec);
	out[16] = 0xf7;
	block_check(out);
	return 1;
}

int reac_box_model_block(const struct reac_box_model *m, enum reac_box_block b,
                         uint8_t out[32])
{
	if (!m || !out)
		return -1;
	memset(out, 0, BLK);

	switch (b) {
	case REAC_BOX_BLOCK_CONFIG:
		return build_config(m, out);
	case REAC_BOX_BLOCK_CC0014:
		memcpy(out, JOIN_0014, BLK);
		return 1;
	case REAC_BOX_BLOCK_CC0013:
		memcpy(out, JOIN_0013, BLK);
		return 1;
	case REAC_BOX_BLOCK_CC0016:
		return build_firmware(m, out);
	case REAC_BOX_BLOCK_CC001A:
		return build_reac_version(m, out);
	case REAC_BOX_BLOCK_IDENT_FIRST:
		/* A family whose selector already names it sends NEITHER fragment; the
		 * row's one flag gates both, and 0 is "this row does not emit that",
		 * which is a fact and not a failure. */
		if (!m->has_identity_record)
			return 0;
		return build_ident_first(m, out);
	case REAC_BOX_BLOCK_IDENT_LAST:
		if (!m->has_identity_record)
			return 0;
		return build_ident_last(m, out);
	default:
		return -1;
	}
}

/* The frame width a row can actually put on the wire — see the header: the braid
 * packs PAIRS, so an output-only row still speaks at the minimum pair while its
 * declaration says zero inputs. An odd or over-wide declaration is refused (0)
 * rather than rounded: a width the frame cannot carry must not reach a builder
 * as a plausible number. */
int reac_box_model_upstream_width(const struct reac_box_model *m)
{
	if (!m || m->in_ch < 0 || m->in_ch > REAC_MAX_CHANNELS || (m->in_ch & 1))
		return 0;
	return m->in_ch < 2 ? 2 : m->in_ch;
}
