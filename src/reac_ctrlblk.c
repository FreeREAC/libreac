// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The 32-byte control block and the scene transfer built on it. See
 * reac/reac_ctrlblk.h for the protocol, the licence reason and the
 * kernel-portability commitments this file is bound by. */

#include <reac/reac_ctrlblk.h>
#include <reac/reac.h>          /* REAC_SAMPLES_PER_PKT, REAC_RESOLUTION */
#include <reac/reac_encode.h>   /* reac_braid_encode — the layout oracle's encode side */
#include <string.h>

/* Frame offsets, shared by the parser and the builders. A REAC frame is
 * dst[6] src[6] ethertype[2] counter[2] type[2] block[32] audio... */
#define ETH_HDR    14
#define CNT_OFF    14
#define TYPE_OFF   16
#define AUDIO_OFF  50
#define DESC_WORD_HI 0x00
#define DESC_WORD_LO 0x7a   /* per-channel descriptor byte observed on the wire */

static inline void put_hdr(uint8_t *f, const uint8_t dst[6], const uint8_t src[6],
                           uint16_t counter, uint8_t t0, uint8_t t1)
{
	memcpy(f, dst, 6);
	memcpy(f + 6, src, 6);
	f[12] = 0x88; f[13] = 0x19;
	f[CNT_OFF] = (uint8_t)(counter & 0xff);
	f[CNT_OFF + 1] = (uint8_t)(counter >> 8);
	f[TYPE_OFF] = t0; f[TYPE_OFF + 1] = t1;
}

void reac_ctrl_block_cksum_stamp(uint8_t block[REAC_CTRL_BLOCK_LEN])
{
	unsigned s = 0;
	for (int i = 0; i < REAC_CTRL_BLOCK_LEN - 1; i++)
		s += block[i];
	block[REAC_CTRL_BLOCK_LEN - 1] = (uint8_t)((256 - (s & 0xff)) & 0xff);
}

void reac_ctrl_checksum_apply(uint8_t *frame)
{
	reac_ctrl_block_cksum_stamp(frame + REAC_CTRL_BLOCK_OFF);
}

int reac_ctrl_checksum_verify(const uint8_t *frame)
{
	unsigned s = 0;
	for (int i = REAC_CTRL_BLOCK_OFF; i < REAC_CTRL_BLOCK_END; i++)
		s += frame[i];
	return (s & 0xff) == 0 ? 0 : -1;
}

/* The nested record checksum sums to 0x80, not to 0. */
void reac_ctrl_record_cksum_stamp(uint8_t *rec, size_t n)
{
	unsigned s = 0;
	for (size_t i = 0; i + 1 < n; i++)
		s += rec[i];
	rec[n - 1] = (uint8_t)((0x80 - s) & 0xff);
}

int reac_ctrl_record_cksum_verify(const uint8_t *rec, size_t n)
{
	unsigned s = 0;
	for (size_t i = 0; i < n; i++)
		s += rec[i];
	return ((s & 0xff) == 0x80) ? 0 : -1;
}

/* ---- the scene push ------------------------------------------------------
 * Every step is the same 34-byte [type|block] shape: cd ea, then the block's
 * four header fields — link, segment, length, opcode — then the payload, block
 * checksum last. The whole transfer is ONE opcode (0x00, bulk) on link 1; what
 * changes step to step is the SEGMENT byte, which is why there is no op field
 * here. The box builds it the same way, from the same four fields.
 *
 * The length a step declares is its PAYLOAD length — the one place block[2:4]
 * counts payload rather than counting from block[4] — and those lengths are
 * exactly what sum to the declared total, so the three sizes and the total are
 * one fact, not four. A body whose lengths do not sum to what the first frame
 * declares leaves the box waiting for bytes that never come.
 *
 * Offsets are into the 34-byte [type|block] template, so block[k] is at k+2. */
#define SCENE_LINK_OFF        2   /* block[0]   the link                       */
#define SCENE_SEG_OFF         3   /* block[1]   FIRST / MIDDLE / LAST          */
#define SCENE_LEN_OFF         4   /* block[2:4] BE payload length              */
#define SCENE_OPCODE_OFF      6   /* block[4]   0x00, bulk, on every step      */
#define SCENE_PAY_OFF         7   /* block[5]   payload, MIDDLE and LAST       */
#define SCENE_HEAD_TOTAL_OFF  7   /* block[5:7] the total; FIRST's payload at
                                   * block[7], two bytes further on           */

int reac_ctrl_build_scene_step(uint8_t blk[34], const uint8_t *body, size_t n,
                               int step)
{
	if (!blk || !body || n != REAC_SCENE_BYTES)
		return -1;
	if (step < 0 || step >= REAC_SCENE_STEPS)
		return -1;

	memset(blk, 0, 34);
	blk[0] = 0xcd; blk[1] = 0xea;

	if (step == 0) {
		blk[SCENE_LINK_OFF] = REAC_LINK_CTRL;
		blk[SCENE_SEG_OFF]  = REAC_SEG_FIRST;
		blk[SCENE_OPCODE_OFF] = REAC_OP_BULK;
		blk[SCENE_LEN_OFF]     = (uint8_t)(REAC_SCENE_HEAD_BYTES >> 8);
		blk[SCENE_LEN_OFF + 1] = (uint8_t)(REAC_SCENE_HEAD_BYTES & 0xff);
		blk[SCENE_HEAD_TOTAL_OFF]     = (uint8_t)(REAC_SCENE_BYTES >> 8);
		blk[SCENE_HEAD_TOTAL_OFF + 1] = (uint8_t)(REAC_SCENE_BYTES & 0xff);
		memcpy(blk + SCENE_HEAD_TOTAL_OFF + 2, body, REAC_SCENE_HEAD_BYTES);
	} else if (step <= REAC_SCENE_CHUNKS) {
		size_t off = REAC_SCENE_HEAD_BYTES +
		             (size_t)(step - 1) * REAC_SCENE_CHUNK_BYTES;
		blk[SCENE_LINK_OFF] = REAC_LINK_CTRL;
		blk[SCENE_SEG_OFF]  = REAC_SEG_MIDDLE;
		blk[SCENE_OPCODE_OFF] = REAC_OP_BULK;
		blk[SCENE_LEN_OFF]     = (uint8_t)(REAC_SCENE_CHUNK_BYTES >> 8);
		blk[SCENE_LEN_OFF + 1] = (uint8_t)(REAC_SCENE_CHUNK_BYTES & 0xff);
		memcpy(blk + SCENE_PAY_OFF, body + off, REAC_SCENE_CHUNK_BYTES);
	} else {
		/* The final chunk fills the SAME 26-byte payload slot as every other one
		 * but declares only REAC_SCENE_TAIL_BYTES of it as body — the transfer
		 * ends mid-slot. The 12 bytes behind the body are a fixed trailer, not
		 * desk state: identical in every LAST frame of both the M-200i and the M-300
		 * establish captures (3/3 each), so they are reproduced rather than
		 * zeroed. Zeroing still satisfies the declared length, but this box has
		 * punished "functionally equivalent" before. */
		static const uint8_t TAIL_TRAILER[12] = {
			0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
			0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
		};
		size_t off = REAC_SCENE_BYTES - REAC_SCENE_TAIL_BYTES;
		blk[SCENE_LINK_OFF] = REAC_LINK_CTRL;
		blk[SCENE_SEG_OFF]  = REAC_SEG_LAST;
		blk[SCENE_OPCODE_OFF] = REAC_OP_BULK;
		blk[SCENE_LEN_OFF]     = (uint8_t)(REAC_SCENE_TAIL_BYTES >> 8);
		blk[SCENE_LEN_OFF + 1] = (uint8_t)(REAC_SCENE_TAIL_BYTES & 0xff);
		memcpy(blk + SCENE_PAY_OFF, body + off, REAC_SCENE_TAIL_BYTES);
		memcpy(blk + SCENE_PAY_OFF + REAC_SCENE_TAIL_BYTES,
		       TAIL_TRAILER, sizeof TAIL_TRAILER);
	}

	reac_ctrl_block_cksum_stamp(blk + 2);
	return 0;
}

int reac_ctrl_scene_set_mac(uint8_t *body, size_t n, const uint8_t mac[6])
{
	if (!body || !mac || n != REAC_SCENE_BYTES)
		return -1;
	memcpy(body + REAC_SCENE_MAC_OFF, mac, 6);
	return 0;
}

int reac_ctrl_scene_build(uint8_t *body, size_t n, const uint8_t mac[6])
{
	if (!body || !mac || n != REAC_SCENE_BYTES)
		return -1;

	memset(body, 0, REAC_SCENE_BYTES);
	memcpy(body + REAC_SCENE_TAG_ID_OFF,   "1234", 4);
	memcpy(body + REAC_SCENE_TAG_SYSP_OFF, "SYSP", 4);
	memcpy(body + REAC_SCENE_TAG_SCEN_OFF, "SCEN", 4);
	memcpy(body + REAC_SCENE_MAC_OFF,      mac,    6);
	return 0;
}

/* ---- head-amp sensitivity ------------------------------------------------
 * One decibel per step over all 56 steps, -10 dBu at 0x00 down to -65 dBu at
 * 0x37, pad off. See reac/reac_ctrlblk.h for the sweep this rests on and for
 * what it replaced — a firmware-derived 56-entry curve whose three
 * duplicate-gain steps do not exist on the metal.
 *
 * Integer hundredths, not floats: this file is written to stay kernel-portable,
 * and a gain curve is exactly the place a float would sneak in. */
#define SENS_REF_CDB   REAC_HEADAMP_SENS_REF_CDB
#define SENS_STEP_CDB  REAC_HEADAMP_SENS_STEP_CDB
#define SENS_PAD_CDB   REAC_HEADAMP_PAD_CDB

int reac_headamp_sens_cdb(uint8_t value, int pad_on)
{
	if (value > REAC_HEADAMP_SENS_MAX)
		value = REAC_HEADAMP_SENS_MAX;
	return SENS_REF_CDB - (int)value * SENS_STEP_CDB + (pad_on ? SENS_PAD_CDB : 0);
}

uint8_t reac_headamp_sens_value_cdb(int cdb, int pad_on)
{
	/* Nearest step, then clamp. `want` is the GAIN asked for above step 0, so a
	 * sensitivity hotter than the box's minimum gain gives a negative want and
	 * must land on 0x00 — C's division truncates toward zero, which would round
	 * -150 to -1 and then clamp, so the negative case is taken first rather than
	 * left to the arithmetic. */
	int want = SENS_REF_CDB + (pad_on ? SENS_PAD_CDB : 0) - cdb;
	if (want <= 0)
		return 0;
	int step = (want + SENS_STEP_CDB / 2) / SENS_STEP_CDB;
	if (step > REAC_HEADAMP_SENS_MAX)
		step = REAC_HEADAMP_SENS_MAX;
	return (uint8_t)step;
}

/* Whole-dB wrappers. Exact, because the step IS a whole dB. */
int reac_headamp_sens_db(uint8_t value, int pad_on)
{
	int c = reac_headamp_sens_cdb(value, pad_on);
	return (c >= 0) ? (c + 50) / 100 : -((-c + 50) / 100);
}

uint8_t reac_headamp_sens_value(int db, int pad_on)
{
	return reac_headamp_sens_value_cdb(db * 100, pad_on);
}

static const char *const KIND_NAME[] = {
	"none", "filler", "scene_transfer", "master_hb", "master_announce",
	"grant", "headamp", "box_hb", "split_announce", "config_announce",
	"group_map", "record_fragment", "link2", "unknown_ctrl",
};

const char *reac_ctrl_kind_name(enum reac_ctrl_kind kind)
{
	unsigned i = (unsigned)kind;
	if (i >= sizeof KIND_NAME / sizeof KIND_NAME[0])
		return "?";
	return KIND_NAME[i];
}

/* The link-1 opcodes, in one place, so the classifier reads as the box's own
 * dispatch does and a new opcode is a row rather than another else-if. */
static enum reac_ctrl_kind kind_of_link1(uint8_t opcode)
{
	switch (opcode) {
	case REAC_OP_BULK:       return REAC_CTRL_SCENE_TRANSFER;
	case REAC_OP_SLOT_MAP:   return REAC_CTRL_MASTER_HB;
	case REAC_OP_GROUP_MAP:  return REAC_CTRL_GROUP_MAP;
	case REAC_OP_BOX_HB:     return REAC_CTRL_BOX_HB;
	case REAC_OP_DECL:
	case REAC_OP_DECL_ALT:
	case REAC_OP_DECL_OTHER: return REAC_CTRL_CONFIG_ANNOUNCE;
	default:                 return REAC_CTRL_UNKNOWN_CTRL;
	}
}

enum reac_ctrl_kind reac_ctrl_parse(const uint8_t *frame, size_t len,
                                    struct reac_ctrl_parsed *out)
{
	struct reac_ctrl_parsed scratch;
	if (!out)
		out = &scratch;      /* the header promises NULL is allowed */
	memset(out, 0, sizeof *out);
	if (len < AUDIO_OFF || frame[12] != 0x88 || frame[13] != 0x19) {
		out->kind = REAC_CTRL_NONE;
		return out->kind;
	}
	memcpy(out->dst, frame, 6);
	memcpy(out->src, frame + 6, 6);
	out->is_broadcast = (memcmp(frame, "\xff\xff\xff\xff\xff\xff", 6) == 0);
	out->counter = (uint16_t)(frame[CNT_OFF] | (frame[CNT_OFF + 1] << 8));

	const uint8_t *block = frame + REAC_CTRL_BLOCK_OFF;
	out->link    = block[0];
	out->seg     = block[1];
	out->blk_len = (uint16_t)((block[2] << 8) | block[3]);
	out->opcode  = block[4];

	const uint8_t t0 = frame[TYPE_OFF], t1 = frame[TYPE_OFF + 1];
	if (t0 == 0x00 && t1 == 0x00) {
		out->kind = REAC_CTRL_FILLER;
		return out->kind;
	}
	if (t0 == 0xcf && t1 == 0xea) {
		out->kind = REAC_CTRL_MASTER_ANNOUNCE;
		return out->kind;
	}
	if (t0 == 0xce && t1 == 0xea) {
		/* A splitter's announce — the split role's own frame type, unicast to
		 * the master ~1/s, block-checksummed like every announce (reac-aes67
		 * REAC-PROTOCOL.md §6/§10.1, source-derived from reacdriver). Never
		 * yet captured on our rig (§14.1: the last unmapped type), so this
		 * names the kind and nothing more — no field decoding until a real
		 * capture grounds the layout. */
		out->kind = REAC_CTRL_SPLIT_ANNOUNCE;
		return out->kind;
	}
	if (t0 != 0xcd || t1 != 0xea) {
		out->kind = REAC_CTRL_UNKNOWN_CTRL;
		return out->kind;
	}

	switch (out->link) {
	case REAC_LINK_CTRL:
		out->kind = kind_of_link1(out->opcode);
		break;
	case REAC_LINK_SECOND:
		out->kind = REAC_CTRL_LINK2;
		break;
	case REAC_LINK_RECORD:
		if (out->seg != REAC_SEG_SINGLE) {
			/* HALF a DT1 record. Its body concatenates with the other
			 * fragment's and the SysEx checksum closes only across both, so
			 * nothing inside it is read here: a tag lifted from a first
			 * fragment is a tag read out of a truncated record. */
			out->kind = REAC_CTRL_RECORD_FRAGMENT;
			break;
		}
		/* A record container. block[14] is the DT1 model-id low byte, block[15]
		 * the command and block[16:18] the register page. TAG 0x0101 is the
		 * console's preamp command — a live M-200 emits ~628 head-amp records
		 * per 14 grants, so a joining slave must not read a knob-turn as its
		 * grant. Every other tag (the join grant, box-ready, identity, the head
		 * mark) stays GRANT. */
		out->dt1_tag = (uint16_t)((block[16] << 8) | block[17]);
		if (block[14] == REAC_DT1_MODEL_LO && block[15] == REAC_DT1_CMD_DT1 &&
		    out->dt1_tag == REAC_DT1_TAG_HEADAMP) {
			out->kind  = REAC_CTRL_HEADAMP;
			out->ch    = block[18];
			out->param = block[19];
			out->value = block[20];
		} else {
			out->kind = REAC_CTRL_GRANT;
		}
		break;
	default:
		out->kind = REAC_CTRL_UNKNOWN_CTRL;
		break;
	}
	return out->kind;
}

/* See the header. The SysEx runs block[9 .. 9+block[8]); its last two bytes are
 * the Roland checksum and 0xf7. For an identity record the command is at
 * block[15], the register page (0x0500) at block[16:18], the address low half at
 * block[18:20], and the payload from block[20] up to the checksum. */
int reac_ctrl_identity_reply(const uint8_t *frame, size_t len, uint16_t *addr_lo,
                             const uint8_t **payload, size_t *payload_len)
{
	if (!frame || !addr_lo || !payload || !payload_len)
		return -1;
	struct reac_ctrl_parsed p;
	if (reac_ctrl_parse(frame, len, &p) != REAC_CTRL_GRANT)
		return 0;                       /* not a single DT1 record container */
	if (p.dt1_tag != REAC_DT1_TAG_IDENTITY)
		return 0;
	const uint8_t *block = frame + REAC_CTRL_BLOCK_OFF;
	if (block[14] != REAC_DT1_MODEL_LO || block[15] != REAC_DT1_CMD_DT1)
		return 0;                       /* an RQ1 poll (0x11), or not a DT1 */
	unsigned sysex_len = block[8];
	/* Preamble(6)+cmd(1)+tag(2)+addr(2)+cksum(1)+f7(1) = 13 with no payload; a
	 * real reply carries at least one payload byte, and the SysEx must fit the
	 * 32-byte block starting at block[9]. */
	if (sysex_len < 14 || (unsigned)(9 + sysex_len) > REAC_CTRL_BLOCK_LEN)
		return 0;
	if (block[9] != 0xf0 || block[9 + sysex_len - 1] != 0xf7)
		return 0;
	*addr_lo = (uint16_t)((block[18] << 8) | block[19]);
	*payload = &block[20];
	*payload_len = (size_t)sysex_len - 13;
	return 1;
}

/* box-width frame length for n_ch inputs */
size_t reac_ctrl_box_frame_len(int n_ch)
{
	return (size_t)AUDIO_OFF + (size_t)n_ch * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION + 2;
}

/* The braided audio region of every box->master frame that carries audio — the
 * upstream FILLER, the broadcast presence-flood AND the cold-connect, because on
 * a real box that region varies every frame (it is live input, NOT static
 * inventory). Slot placement is plain ascending; a real M-5000 expects exactly
 * this from a box's return.
 *
 * The loop itself moved to libreac on 2026-07-29 as reac_braid_encode()
 * (<reac/reac_encode.h>): it was byte-for-byte the same loop as the downstream
 * encoder's, which is the point — the braid is the REAC wire format in BOTH
 * directions (task #108, the ex-"FPGA scramble" of task #61), and it is the
 * exact inverse of reac_upstream_decode(). What stayed here is everything the
 * layout is not: the 32-byte control block, its two nested checksums, the
 * box-model matrix and the frame envelope, all of which are handshake state
 * owned by the role FSM. Passing n_ch as both the frame width and the plane
 * count preserves the pre-move behaviour exactly (this builder is always given
 * one plane per box input).
 */

/* ---- FIXED box-model matrix (byte-verified real announce blocks) ----
 * Role decides authority (docs/REAC-BOX-STATE-DIAGRAM.md): as a SLAVE (we ARE a
 * stagebox) this matrix is LAW — we pick a row and emit its announce verbatim. As
 * a MASTER (we ARE a mixer) the box's announce on the wire is the truth and this
 * matrix is only a default. Each row is a real box's captured config-announce
 * (selector byte = displayed model family; sum mod 256 == 0 with its trailing
 * check byte), plus, for the 0x84 family, the identity record that names the
 * exact model. All blocks byte-matched to matrix-m200/m5000-s1608 / -s0808. */
static const struct reac_box_model BOX_MODELS[] = {
	{ .token = "s1608", .display = "S-1608 (16 in / 8 out)", .in_ch = 16, .out_ch = 8,
	  .config_block = {
		0x01, 0x03, 0x00, 0x10, 0x82, 0x00, 0x00, 0x02,
		0x02, 0x02, 0x02, 0x02, 0x01, 0x01, 0x03, 0x03,
		0x03, 0x03, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c },
	  .has_identity_record = 0,   /* named by the declaration's constant */
	  .cc0014 = {
		0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe,
		0x0f, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x01, 0x00, 0x06, 0x00, 0x01, 0x00, 0x78, 0xf7,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	  .cc0013 = {
		0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe,
		0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x03, 0x02, 0x00, 0x01, 0x00, 0x7a, 0xf7, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 },
	  .cc0016 = {
		0x04, 0x03, 0x00, 0x16, 0x00, 0x02, 0x00, 0xfe,
		0x11, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00,
		0x77, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc },
	  .cc001a = {
		0x04, 0x03, 0x00, 0x1a, 0x00, 0x02, 0x00, 0xfe,
		0x15, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x02,
		0x00, 0x03, 0x00, 0x02, 0x6e, 0xf7, 0x00, 0xf4 },
	},
	{ .token = "s0808", .display = "S-0808 (8 in / 8 out)", .in_ch = 8, .out_ch = 8,
	  .config_block = {
		0x01, 0x03, 0x00, 0x10, 0x84, 0x00, 0x00, 0x00,
		0x02, 0x02, 0x01, 0x01, 0x03, 0x03, 0x03, 0x03,
		0x03, 0x03, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a },
	  .has_identity_record = 1,   /* the 0x84 constant needs the name */
	  .identity_first = {
		0x04, 0x01, 0x00, 0x1b, 0x00, 0x02, 0x00, 0xfe,
		0x16, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x10, 0x00, 0x01, 0x53, 0x2d, 0x30,   /* "S-0" */
		0x38, 0x30, 0x38, 0x00, 0x00, 0x00, 0x00, 0x05 },  /* "808" */
	  .cc0014 = {         /* 0014/0013 match the S-1608's (model-generic so far) */
		0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe,
		0x0f, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x01, 0x00, 0x06, 0x00, 0x01, 0x00, 0x78, 0xf7,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	  .cc0013 = {
		0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe,
		0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x03, 0x02, 0x00, 0x01, 0x00, 0x7a, 0xf7, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 },
	  .cc0016 = {         /* S-0808's inventory differs from S-1608's */
		0x04, 0x03, 0x00, 0x16, 0x00, 0x02, 0x00, 0xfe,
		0x11, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x03,
		0x77, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc },
	  .cc001a = {
		0x04, 0x03, 0x00, 0x1a, 0x00, 0x02, 0x00, 0xfe,
		0x15, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01,
		0x00, 0x00, 0x00, 0x00, 0x74, 0xf7, 0x00, 0xf4 },
	  .identity_last = {
		0x04, 0x02, 0x00, 0x0d, 0x00, 0x02, 0x00, 0xfe,
		0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1a,
		0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd4 } },
	/* S-4000S — also 0x84 family but sends NO identity record (0x84's DEFAULT
	 * desk label IS "S-4000S"). Its config descriptor + 0016/001a
	 * inventory are distinct. Byte-verified from a real S-4000S cold boot on an
	 * M-5000 (s4000s-coldboot-m5000-2026-07-12, box c4:06:80). NOTE: captured on
	 * OHRCA (frames +2 CRC trailer); the control blocks below are generation-
	 * independent, but emulating on an OHRCA desk needs the upstream +2 (W4). */
	{ .token = "s4000s", .display = "S-4000S (32 in / 8 out)", .in_ch = 32, .out_ch = 8,
	  .config_block = {
		0x01, 0x03, 0x00, 0x10, 0x84, 0x00, 0x00, 0x00,
		0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
		0x01, 0x01, 0x03, 0x03, 0x00, 0x03, 0x00, 0x00,
		0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c },
	  .has_identity_record = 0,   /* the 0x84 constant already reads S-4000S */
	  .cc0014 = {
		0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe,
		0x0f, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x01, 0x00, 0x06, 0x00, 0x01, 0x00, 0x78, 0xf7,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	  .cc0013 = {
		0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe,
		0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x03, 0x02, 0x00, 0x01, 0x00, 0x7a, 0xf7, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 },
	  .cc0016 = {
		0x04, 0x03, 0x00, 0x16, 0x00, 0x02, 0x00, 0xfe,
		0x11, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x00, 0x00, 0x02, 0x05, 0x00, 0x00,
		0x74, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc },
	  .cc001a = {
		0x04, 0x03, 0x00, 0x1a, 0x00, 0x02, 0x00, 0xfe,
		0x15, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x02,
		0x00, 0x01, 0x00, 0x02, 0x70, 0xf7, 0x00, 0xf4 },
	},
};

const struct reac_box_model *reac_box_model_table(size_t *count)
{
	if (count) *count = sizeof(BOX_MODELS) / sizeof(BOX_MODELS[0]);
	return BOX_MODELS;
}

const struct reac_box_model *reac_box_model_by_token(const char *token)
{
	size_t n = sizeof(BOX_MODELS) / sizeof(BOX_MODELS[0]);
	for (size_t i = 0; i < n; i++)
		if (token && strcmp(BOX_MODELS[i].token, token) == 0)
			return &BOX_MODELS[i];
	return NULL;
}

/* Map an input width to its matrix row (each verified width is one model). Falls
 * back to S-1608 for widths not in the matrix so the pure builders never fault. */
const struct reac_box_model *reac_box_model_by_channels(int in_ch)
{
	size_t n = sizeof(BOX_MODELS) / sizeof(BOX_MODELS[0]);
	for (size_t i = 0; i < n; i++)
		if (BOX_MODELS[i].in_ch == in_ch)
			return &BOX_MODELS[i];
	return &BOX_MODELS[0];   /* default: S-1608 */
}

const struct reac_box_model *reac_ctrl_identify_box(const uint8_t *frame, size_t len)
{
	/* Recognize the connected box's MODEL from its config-announce
	 * (link 1, opcode 0x82 / 0x84 / 0x80) by matching the 32-byte descriptor
	 * block against the
	 * fixed matrix. Each row's config_block is unique (selector + descriptor:
	 * S-1608 0x82; S-0808 / S-4000S both 0x84 but distinct descriptors), so an
	 * exact block match uniquely names the model. NULL = not a config-announce,
	 * or no known model -> caller falls back to the frame's own descriptor/width. */
	if (len < REAC_CTRL_BLOCK_OFF + 32)               return NULL;
	if (frame[12] != 0x88 || frame[13] != 0x19)       return NULL;   /* 0x8819 */
	if (frame[16] != 0xcd || frame[17] != 0xea)       return NULL;   /* cdea   */
	struct reac_ctrl_parsed p;
	if (reac_ctrl_parse(frame, len, &p) != REAC_CTRL_CONFIG_ANNOUNCE)
		return NULL;
	size_t n; const struct reac_box_model *t = reac_box_model_table(&n);
	for (size_t i = 0; i < n; i++)
		if (memcmp(frame + REAC_CTRL_BLOCK_OFF, t[i].config_block, 32) == 0)
			return &t[i];
	return NULL;
}

/* ---- The control-frame scaffold + descriptor table ------------------------
 *
 * Every frame reac-pw emits is the same six-step ritual: zero the frame, stamp
 * the ethernet + REAC header, lay the 32-byte control block [18:50], optionally
 * place braided audio at [50:], stamp the checksums, write the C2 EA end marker.
 * What differs between frames is only WHERE the block comes from, whether audio
 * rides along, how wide the frame is and which checksums apply — so those four
 * axes are a TABLE ROW and the ritual is one function. Adding a control frame is
 * a row in CTRL_FRAMES[], not another copied builder.
 *
 * The public builders stay exactly what they were on the wire: this is a pure
 * refactor, byte-for-byte (the goldens are the oracle). */

/* Where a row's 32-byte control block comes from. The BLOCK_* names that select
 * a box-model member are resolved by ctrl_model_block(), so a row names the
 * member and the compiler checks it — no offsets, no casts. */
enum ctrl_block {
	BLOCK_ZERO = 0,   /* left zero — the broadcast presence-flood          */
	BLOCK_DESC,       /* the 00 7a per-slot descriptor — upstream FILLER   */
	BLOCK_TMPL,       /* the row's own literal template                    */
	BLOCK_CONFIG,     /* matrix: config-announce   cdea 01 03 0010         */
	BLOCK_IDENT_FIRST,/* matrix: identity record, link 4 FIRST fragment    */
	BLOCK_CC0014,     /* matrix: cold-connect      cdea 04 03 0014         */
	BLOCK_CC0013,     /* matrix: cold-connect      cdea 04 03 0013         */
	BLOCK_CC0016,     /* matrix: cold-connect      cdea 04 03 0016         */
	BLOCK_CC001A,     /* matrix: cold-connect      cdea 04 03 001a         */
	BLOCK_IDENT_LAST, /* matrix: identity record, link 4 LAST fragment     */
};

/* Frame width. A box->master frame is 50 + 36*width + 2; a master->box frame is
 * the fixed downstream width. */
enum ctrl_len {
	LEN_ARG_WIDTH = 0,   /* the caller's n_ch (validated: even, 2..40)     */
	LEN_MODEL_WIDTH,     /* the matrix row's input width                   */
	LEN_DOWNSTREAM,      /* REAC_FRAME_BYTES (master direction)            */
};

/* Which models emit this frame at all: a row names the matrix flag and
 * ctrl_gate_ok() reads it. Both identity fragments name the SAME flag, because
 * they are one record and half of it is not a message. */
enum ctrl_gate {
	GATE_ALWAYS = 0,
	GATE_HAS_IDENTITY,
};

/* Checksum policy. CKSUM_RECORD means the block carries a Roland DT1 record,
 * whose INNER (sum-to-0x80) checksum lies INSIDE the OUTER (sum-to-0) block
 * checksum — see ctrl_finish() for why the order is the row's whole story. */
enum ctrl_cksum {
	CKSUM_NONE = 0,   /* emitted raw (byte-verified blocks, FILLER)        */
	CKSUM_BLOCK,      /* the OUTER block checksum at [49]                  */
	CKSUM_RECORD,     /* an INNER DT1 record, then the OUTER block         */
};

struct ctrl_frame {
	uint8_t type0, type1;      /* the frame type word at [16:18]            */
	uint8_t block;             /* enum ctrl_block — the block's source      */
	const uint8_t *tmpl;       /* BLOCK_TMPL: the literal block bytes       */
	uint8_t arg_off, arg_len;  /* caller-supplied bytes, block-relative     */
	uint8_t rec_off, rec_len;  /* CKSUM_RECORD: the DT1 record, block-rel.  */
	uint8_t cksum;             /* enum ctrl_cksum                           */
	uint8_t len;               /* enum ctrl_len                             */
	uint8_t gate;              /* enum ctrl_gate                            */
	uint8_t audio;             /* place braided audio at [50:]              */
};

/* The box heartbeat's control block: cdea 01 03 0001, selector 0x81 keep-alive
 * (0x00 is the explicit disconnect). Checksum byte 0x7a on the wire. */
static const uint8_t TMPL_BOX_HB[REAC_CTRL_BLOCK_LEN] = {
	0x01, 0x03, 0x00, 0x01, 0x81,
};

/* The head-amp record container, byte-truthed against a live M-200
 * (m200-headamp-re/ctl2.pcap): cdea 04 03, BE len 0x0013, the f0 41 0a Roland
 * DT1 preamble (block[8] is the preamble length echo, oplen - 5), the 12 12
 * record marker and TAG 01 01 = head-amp. The three zero bytes at block[18:21]
 * are the CH PARAM VALUE window the caller fills; block[21] is the record's
 * INNER checksum, stamped by the scaffold; block[22] is the f7 terminator. */
static const uint8_t TMPL_HEADAMP[REAC_CTRL_BLOCK_LEN] = {
	0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe,
	0x13 - 5, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
	0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0xf7,
};
#define HEADAMP_ARG_OFF 18   /* CH PARAM VALUE, block-relative (frame [36:39]) */
#define HEADAMP_ARG_LEN  3
#define HEADAMP_REC_OFF 16   /* TAG..CKSUM,      block-relative (frame [34:40]) */
#define HEADAMP_REC_LEN  6

static const uint8_t *ctrl_model_block(const struct reac_box_model *m,
                                       enum ctrl_block b)
{
	switch (b) {
	case BLOCK_CONFIG: return m->config_block;
	case BLOCK_IDENT_FIRST: return m->identity_first;
	case BLOCK_CC0014: return m->cc0014;
	case BLOCK_CC0013: return m->cc0013;
	case BLOCK_CC0016: return m->cc0016;
	case BLOCK_CC001A: return m->cc001a;
	case BLOCK_IDENT_LAST:  return m->identity_last;
	default:           return NULL;   /* not a matrix block */
	}
}

static int ctrl_gate_ok(const struct reac_box_model *m, enum ctrl_gate g)
{
	switch (g) {
	case GATE_HAS_IDENTITY: return m->has_identity_record;
	default:             return 1;
	}
}

/* Lay a row's 32-byte control block [18:50] over `frame`, then overwrite the
 * row's argument window with the caller's bytes. The block is zeroed first, so
 * this is safe over an already-built frame: the audio [50:], the counter and the
 * ethernet header are untouched. */
static void ctrl_lay_block(uint8_t *frame, const struct ctrl_frame *f,
                           const struct reac_box_model *m, const uint8_t *args)
{
	uint8_t *block = frame + REAC_CTRL_BLOCK_OFF;
	const uint8_t *from;

	memset(block, 0, REAC_CTRL_BLOCK_LEN);
	switch ((enum ctrl_block)f->block) {
	case BLOCK_ZERO:
		break;
	case BLOCK_DESC:
		for (int k = 0; k < REAC_CTRL_BLOCK_LEN / 2; k++) {
			block[2 * k]     = DESC_WORD_HI;
			block[2 * k + 1] = DESC_WORD_LO;
		}
		break;
	case BLOCK_TMPL:
		memcpy(block, f->tmpl, REAC_CTRL_BLOCK_LEN);
		break;
	default:
		from = ctrl_model_block(m, (enum ctrl_block)f->block);
		if (from)
			memcpy(block, from, REAC_CTRL_BLOCK_LEN);
		break;
	}
	if (args && f->arg_len)
		memcpy(block + f->arg_off, args, f->arg_len);
}

/* THE CHECKSUM ORDER, MADE STRUCTURAL.
 *
 * An op-0403 record carries a Roland DT1 checksum (INNER, sum-to-0x80) INSIDE
 * the REAC control block's own checksum (OUTER, sum-to-0). The inner byte is one
 * of the bytes the outer sum covers, so it MUST be stamped first — a record
 * finished with only the block helper, or with the block helper first, looks
 * perfect on the wire and the box rejects it.
 *
 * This function is the only place in reac_ctrl that stamps either checksum, and
 * CKSUM_RECORD falls through into CKSUM_BLOCK. A table row therefore cannot ask
 * for the outer checksum alone on a frame that carries a record, and cannot ask
 * for them in the wrong order: the order is not a convention a builder has to
 * remember, it is the only path through the switch. */
static void ctrl_finish(uint8_t *frame, const struct ctrl_frame *f)
{
	switch ((enum ctrl_cksum)f->cksum) {
	case CKSUM_RECORD:
		reac_ctrl_record_cksum_stamp(frame + REAC_CTRL_BLOCK_OFF + f->rec_off,
		                             f->rec_len);
		/* fall through - the outer checksum covers the byte just stamped */
	case CKSUM_BLOCK:
		reac_ctrl_block_cksum_stamp(frame + REAC_CTRL_BLOCK_OFF);
		break;
	case CKSUM_NONE:
		break;
	}
}

/* The six-step ritual, once. Returns the frame length, or 0 when the row is not
 * emitted for this model / the width is not a real box width. */
static size_t ctrl_emit(uint8_t *out, const struct ctrl_frame *f,
                        const uint8_t dst[6], const uint8_t src[6],
                        uint16_t counter, int n_ch, const uint8_t *args,
                        float *const *planar, int ns)
{
	/* The braid packs channel PAIRS: box widths are even, 2..40 (628 B at 16,
	 * 340 B at 8). Rows sized from the matrix carry a verified width already. */
	if (f->len == LEN_ARG_WIDTH &&
	    (n_ch < 2 || n_ch > REAC_MAX_CHANNELS || (n_ch & 1)))
		return 0;

	const struct reac_box_model *m = reac_box_model_by_channels(n_ch);
	if (!ctrl_gate_ok(m, (enum ctrl_gate)f->gate))
		return 0;

	size_t len = (f->len == LEN_DOWNSTREAM)
	           ? (size_t)REAC_FRAME_BYTES
	           : reac_ctrl_box_frame_len(f->len == LEN_MODEL_WIDTH ? m->in_ch : n_ch);

	memset(out, 0, len);
	put_hdr(out, dst, src, counter, f->type0, f->type1);
	ctrl_lay_block(out, f, m, args);
	if (f->audio)
		reac_braid_encode(out + AUDIO_OFF, n_ch, planar, n_ch, ns);
	ctrl_finish(out, f);
	out[len - 2] = REAC_END_MARKER_0;
	out[len - 1] = REAC_END_MARKER_1;
	return len;
}

/* Lay a row over an ALREADY-BUILT frame: the type word [16:18] and the control
 * block [18:50] change, the audio [50:], the counter and the C2 EA tail the
 * frame already carries are preserved. Shares ctrl_lay_block + ctrl_finish with
 * ctrl_emit, so an overlaid record gets its two checksums in the same order a
 * freshly built one does — the stamp path cannot drift from the build path. */
static void ctrl_stamp(uint8_t *frame, const struct ctrl_frame *f, const uint8_t *args)
{
	frame[TYPE_OFF] = f->type0;
	frame[TYPE_OFF + 1] = f->type1;
	ctrl_lay_block(frame, f, NULL, args);
	ctrl_finish(frame, f);
}

/* The table. One row per control frame; the evidence for each block lives with
 * the bytes (TMPL_* here, or the box-model matrix above). */
enum ctrl_frame_id {
	CTRL_BOX_HB = 0,
	CTRL_UPSTREAM_FILLER,
	CTRL_FLOOD_FILLER,
	CTRL_CONFIG_ANNOUNCE,
	CTRL_IDENT_FIRST,
	CTRL_COLDCONNECT,
	CTRL_COLDCONNECT_0013,
	CTRL_COLDCONNECT_0016,
	CTRL_COLDCONNECT_001A,
	CTRL_IDENT_LAST,
	CTRL_HEADAMP,
	CTRL_FRAME_COUNT,
};

/* Rows CTRL_CONFIG_ANNOUNCE..CTRL_IDENT_LAST are the RECONSTRUCTED JOIN frames
 * (experimental, not byte-verified as a SEQUENCE): each block is byte-matched to
 * a real capture, but the order and timing a box emits them in is reconstructed
 * from REAC-CONNECTION-FSM.md, not observed end to end. */
static const struct ctrl_frame CTRL_FRAMES[CTRL_FRAME_COUNT] = {
	/* The box keep-alive, in a box-width audio slot. Checksum byte 0x7a on the
	 * wire (reac-captures/wired-reac-a-bothdirs) — the test cross-checks it. */
	[CTRL_BOX_HB] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_TMPL, .tmpl = TMPL_BOX_HB,
		.cksum = CKSUM_BLOCK, .len = LEN_ARG_WIDTH },
	/* The established unicast upstream: the 32-byte descriptor [18:50] = 00 7a per
	 * slot (16 slots), as the real box, over audio in the BRAIDED layout (resolved
	 * 2026-07-10, task #108). FILLER (type 00 00) is checksum-exempt. */
	[CTRL_UPSTREAM_FILLER] = {
		.type0 = 0x00, .type1 = 0x00, .block = BLOCK_DESC,
		.cksum = CKSUM_NONE, .len = LEN_ARG_WIDTH, .audio = 1 },
	/* The presence-flood FILLER (broadcast, unlinked): counter + type 00 00 + a
	 * ZERO control block [18:50] (no 0x7a per-slot descriptor) + LIVE audio
	 * [50:626] + end marker. Verified on the wire
	 * (m200-s1608-realbox-establish-2026-07-11.pcap): a real S-1608's cold-boot
	 * flood carries a zero control block but a LIVE audio region (it varies every
	 * frame) — it is NOT an all-zero payload. The 0x7a descriptor is what
	 * distinguishes the ESTABLISHED unicast upstream from this broadcast announce;
	 * the audio itself is present in both. */
	[CTRL_FLOOD_FILLER] = {
		.type0 = 0x00, .type1 = 0x00, .block = BLOCK_ZERO,
		.cksum = CKSUM_NONE, .len = LEN_ARG_WIDTH, .audio = 1 },
	/* The config-announce (cdea 01 03 0010) — the SETUP DECLARATION the master
	 * enrols the box from; the selector byte sets the displayed model family. The
	 * verified blocks already sum to 0, so the outer stamp is a no-op that keeps
	 * the invariant rather than a correction. */
	[CTRL_CONFIG_ANNOUNCE] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_CONFIG,
		.cksum = CKSUM_BLOCK, .len = LEN_MODEL_WIDTH },
	/* The identity record's FIRST fragment: the DT1 preamble, TAG 0x0500 and the
	 * ASCII model name, so the desk shows "S-0808" and not the family constant's
	 * default label. Whoever emits this MUST emit CTRL_IDENT_LAST after it — the
	 * SysEx checksum lives there and closes over both. Models named by the
	 * declaration's constant alone emit neither. */
	[CTRL_IDENT_FIRST] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_IDENT_FIRST,
		.cksum = CKSUM_NONE, .len = LEN_MODEL_WIDTH, .gate = GATE_HAS_IDENTITY },
	/* The cold-connect escalation a real S-1608 sends: 0014 -> 0013 -> 0016 ->
	 * 001a, each the model's 32-byte control block over LIVE audio. The block's
	 * [38:66] region is frame[52:80] and is AUDIO, not device inventory — on a
	 * real box it varies every frame (verified 2026-07-11,
	 * m200-s1608-realbox-establish). The master needs no inventory tail: it learns
	 * the box from the L2 source and echoes THIS block back verbatim as its grant,
	 * so a cold-connect is the control block over live audio, exactly like the
	 * unicast upstream but with cdea 04 03 replacing the 0x7a descriptor.
	 *
	 * 0014: the 0x41 at block[10] is descriptor DATA, not a MAC tail — the block
	 * is MAC-independent. Sum(block) mod 256 == 0 holds as captured, so the outer
	 * stamp is a no-op that keeps the invariant.
	 * 0013: the variant a real box INTERLEAVES with the 0014 (S-1608 cold boot,
	 * m200-s1608-BIDIR-reboot-2026-07-11); block[31]=0x02 trailer, and the
	 * captured block sums to 0xfe mod 256 — NOT sum-to-0, which is the evidence
	 * that the cold-connect is not checksum-validated the way 0014 happens to be.
	 * It is therefore emitted RAW, as are 0016 and 001a.
	 * 0016: a MODEL-specific inventory block the mixer uses to identify the box.
	 * 001a: the fullest MODEL-specific box inventory.
	 * All four byte-matched per model (matrix-m200-s1608 / -s0808, 2026-07-11;
	 * S-4000S from s4000s-coldboot-m5000-2026-07-12). */
	[CTRL_COLDCONNECT] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_CC0014,
		.cksum = CKSUM_BLOCK, .len = LEN_ARG_WIDTH, .audio = 1 },
	[CTRL_COLDCONNECT_0013] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_CC0013,
		.cksum = CKSUM_NONE, .len = LEN_ARG_WIDTH, .audio = 1 },
	[CTRL_COLDCONNECT_0016] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_CC0016,
		.cksum = CKSUM_NONE, .len = LEN_ARG_WIDTH, .audio = 1 },
	[CTRL_COLDCONNECT_001A] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_CC001A,
		.cksum = CKSUM_NONE, .len = LEN_ARG_WIDTH, .audio = 1 },
	/* The identity record's LAST fragment: the SysEx checksum that closes over
	 * both fragments, and the f7 that ends the record. Emitted raw
	 * (byte-verified, matrix-m200-s0808). */
	[CTRL_IDENT_LAST] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_IDENT_LAST,
		.cksum = CKSUM_NONE, .len = LEN_MODEL_WIDTH, .gate = GATE_HAS_IDENTITY },
	/* The console-side preamp command, master->box at the downstream width. The
	 * ONLY row that carries a DT1 record — and naming CKSUM_RECORD is all it has
	 * to do: ctrl_finish() stamps the inner checksum and then the outer one, in
	 * that order, because that is the only path through its switch. */
	[CTRL_HEADAMP] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_TMPL, .tmpl = TMPL_HEADAMP,
		.arg_off = HEADAMP_ARG_OFF, .arg_len = HEADAMP_ARG_LEN,
		.rec_off = HEADAMP_REC_OFF, .rec_len = HEADAMP_REC_LEN,
		.cksum = CKSUM_RECORD, .len = LEN_DOWNSTREAM },
};

size_t reac_ctrl_build_box_hb(uint8_t *out, const uint8_t master[6],
                              const uint8_t src[6], uint16_t counter, int n_ch)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_BOX_HB], master, src, counter,
	                 n_ch, NULL, NULL, 0);
}

size_t reac_ctrl_build_upstream_filler(uint8_t *out, const uint8_t master[6],
                                       const uint8_t src[6], uint16_t counter,
                                       int n_ch, float *const *planar, int ns)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_UPSTREAM_FILLER], master, src,
	                 counter, n_ch, NULL, planar, ns);
}

size_t reac_ctrl_build_flood_filler(uint8_t *out, const uint8_t bcast[6],
                                    const uint8_t src[6], uint16_t counter,
                                    int n_ch, float *const *planar, int ns)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_FLOOD_FILLER], bcast, src,
	                 counter, n_ch, NULL, planar, ns);
}

size_t reac_ctrl_build_config_announce(uint8_t *out, const uint8_t master[6],
                                       const uint8_t src[6], uint16_t counter, int in_ch)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_CONFIG_ANNOUNCE], master, src,
	                 counter, in_ch, NULL, NULL, 0);
}

size_t reac_ctrl_build_identity_first(uint8_t *out, const uint8_t master[6],
                                      const uint8_t src[6], uint16_t counter, int in_ch)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_IDENT_FIRST], master, src,
	                 counter, in_ch, NULL, NULL, 0);
}

size_t reac_ctrl_build_coldconnect(uint8_t *out, const uint8_t master[6],
                                   const uint8_t src[6], uint16_t counter,
                                   int n_ch, float *const *planar, int ns)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_COLDCONNECT], master, src,
	                 counter, n_ch, NULL, planar, ns);
}

size_t reac_ctrl_build_coldconnect_0013(uint8_t *out, const uint8_t master[6],
                                        const uint8_t src[6], uint16_t counter,
                                        int n_ch, float *const *planar, int ns)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_COLDCONNECT_0013], master, src,
	                 counter, n_ch, NULL, planar, ns);
}

size_t reac_ctrl_build_coldconnect_0016(uint8_t *out, const uint8_t master[6],
                                        const uint8_t src[6], uint16_t counter,
                                        int n_ch, float *const *planar, int ns)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_COLDCONNECT_0016], master, src,
	                 counter, n_ch, NULL, planar, ns);
}

size_t reac_ctrl_build_coldconnect_001a(uint8_t *out, const uint8_t master[6],
                                        const uint8_t src[6], uint16_t counter,
                                        int n_ch, float *const *planar, int ns)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_COLDCONNECT_001A], master, src,
	                 counter, n_ch, NULL, planar, ns);
}

size_t reac_ctrl_build_identity_last(uint8_t *out, const uint8_t master[6],
                                     const uint8_t src[6], uint16_t counter, int in_ch)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_IDENT_LAST], master, src,
	                 counter, in_ch, NULL, NULL, 0);
}

/* ---- Head-amp source control (link 4 SINGLE, record TAG 0x0101) ---- */

/* param/value validity for a head-amp record (phantom/pad are boolean, SENS is
 * 0x00..0x37). Shared by the fresh-frame builder and the in-place stamp. */
static int headamp_args_ok(uint8_t param, uint8_t value)
{
	switch (param) {
	case REAC_HEADAMP_PHANTOM:
	case REAC_HEADAMP_PAD:
		return value <= 0x01;
	case REAC_HEADAMP_SENS:
		return value <= REAC_HEADAMP_SENS_MAX;
	default:
		return 0;
	}
}

size_t reac_ctrl_build_headamp(uint8_t *out, const uint8_t master[6],
                               const uint8_t src[6], uint16_t counter,
                               uint8_t ch, uint8_t param, uint8_t value)
{
	/* A real console BROADCASTS these interleaved in its stream, so the caller
	 * passes the broadcast MAC like every builder's first MAC arg. */
	const uint8_t args[HEADAMP_ARG_LEN] = { ch, param, value };

	if (!headamp_args_ok(param, value))
		return 0;
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_HEADAMP], master, src, counter,
	                 0, args, NULL, 0);
}

int reac_ctrl_stamp_headamp(uint8_t *frame, uint8_t ch, uint8_t param, uint8_t value)
{
	/* Overlay a head-amp record onto an already-built downstream frame (the
	 * MASTER-role emit path stamps it over a FILLER slot — see reac_headamp_tx).
	 * Only the type [16:18] + control block [18:50] change; the audio, counter and
	 * C2/EA tail the frame already carries are preserved. Returns -1 on a bad
	 * param/value, leaving the frame untouched. */
	const uint8_t args[HEADAMP_ARG_LEN] = { ch, param, value };

	if (!headamp_args_ok(param, value))
		return -1;
	ctrl_stamp(frame, &CTRL_FRAMES[CTRL_HEADAMP], args);
	return 0;
}

const char *reac_headamp_param_name(uint8_t param)
{
	switch (param) {
	case REAC_HEADAMP_PHANTOM: return "phantom";
	case REAC_HEADAMP_PAD:     return "pad";
	case REAC_HEADAMP_SENS:    return "SENS";
	default:                   return "?";
	}
}

int reac_ctrl_headamp_record_verify(const uint8_t *frame)
{
	/* The inner record is TAG(2) CH PARAM VALUE CKSUM at frame[34..39]; the
	 * console builds CKSUM so the six bytes sum to 0x80 mod 256 (byte-verified
	 * on the M-200, m200-headamp-re/DECODE.md). A frame that fails this carries a
	 * corrupted preamp record and its CH/PARAM/VALUE must not be trusted.
	 *
	 * ONLY ON A COMPLETE RECORD. A link-4 frame whose segment is not SINGLE
	 * carries HALF a record, and the SysEx checksum of a split record closes
	 * across BOTH fragments — 358 mod 128 = 102 and 128 - 102 = 0x1a, the byte
	 * that arrives in the second one. Summing six bytes of a first fragment
	 * tests an arithmetic identity that was never meant to hold there, so the
	 * answer would be a fail with no meaning. Refuse instead. */
	if (frame[REAC_CTRL_BLOCK_OFF] != REAC_LINK_RECORD ||
	    frame[REAC_CTRL_BLOCK_OFF + 1] != REAC_SEG_SINGLE)
		return -1;
	return reac_ctrl_record_cksum_verify(frame + 34, 6);
}

/* cdea 04 03 0014, record 12 12 01 00: the master's ACK of the box's join params. */
static const uint8_t GRANT_HEAD_ACK[34] = {
	0xcd, 0xea, 0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe, 0x0f, 0xf0,
	0x41, 0x0a, 0x00, 0x00, 0x12, 0x12, 0x01, 0x00, 0x06, 0x00, 0x01, 0x00,
	0x78, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* cdea 04 03 0014, record 12 12 00 00: the marker that separates the first
 * channel's group-A records from the group-B block. */
static const uint8_t GRANT_HEAD_MARK[34] = {
	0xcd, 0xea, 0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe, 0x0f, 0xf0,
	0x41, 0x0a, 0x00, 0x00, 0x12, 0x12, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
	0x7d, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* GROUP B — the fixed 6-record constant (marker 12 11, TAG 05 00), byte-identical
 * across 8/16/32-input boxes: (ch,sub,val) = (00,00,04) (06,00,08) (10,00,11)
 * (10,11,09) (11,00,11) (11,11,09). Unchanged from the tables it replaces. */
#define REAC_GRANT_GROUPB_LEN 6
static const uint8_t GRANT_GROUPB[REAC_GRANT_GROUPB_LEN][34] = {
	{ 0xcd, 0xea, 0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe, 0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x11, 0x05, 0x00, 0x00, 0x00, 0x04, 0x77, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 },
	{ 0xcd, 0xea, 0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe, 0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x11, 0x05, 0x00, 0x06, 0x00, 0x08, 0x6d, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 },
	{ 0xcd, 0xea, 0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe, 0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x11, 0x05, 0x00, 0x10, 0x00, 0x11, 0x5a, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 },
	{ 0xcd, 0xea, 0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe, 0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x11, 0x05, 0x00, 0x10, 0x11, 0x09, 0x51, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 },
	{ 0xcd, 0xea, 0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe, 0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x11, 0x05, 0x00, 0x11, 0x00, 0x11, 0x59, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 },
	{ 0xcd, 0xea, 0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe, 0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x11, 0x05, 0x00, 0x11, 0x11, 0x09, 0x50, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 },
};

/* Emit ONE group-A record via the proven head-amp builder. We build into a scratch
 * frame and lift its [16:50] rather than re-deriving the record bytes: that keeps
 * reac_ctrl_stamp_headamp — the function already byte-verified against a real
 * M-200, including BOTH nested checksums — as the single source of these bytes. */
static int put_groupa(uint8_t row[34], uint8_t ch, uint8_t param, uint8_t value)
{
	uint8_t scratch[REAC_FRAME_BYTES];
	memset(scratch, 0, sizeof scratch);
	if (reac_ctrl_stamp_headamp(scratch, ch, param, value) != 0)
		return -1;
	memcpy(row, scratch + 16, 34);   /* [16:50] = type[2] + control block[32] */
	return 0;
}

/* ---- the grant sweep's SHAPE ---------------------------------------------
 * The enrolment burst a master sends a box: which records, in which order, with
 * which markers. That is protocol and lives here. WHICH SLOTS a box gets, and
 * what phantom/pad/SENS each one should carry, are the daemon's decisions and
 * stay there — the caller hands the values in.
 *
 * Group A is the head-amp push: marker 12 12, TAG 01 01, one record per allocated
 * channel per parameter, sub-phase 00/01/02 = phantom/pad/SENS. Group B is the
 * fixed six-record constant, marker 12 11, TAG 05 00.
 */
int reac_ctrl_build_grant_sweep(uint8_t sweep[][34], int max, uint8_t base,
                                int width, const uint8_t *values)
{
	if (!sweep || !values || width <= 0)
		return -1;
	if ((int)base + width > REAC_HEADAMP_MAX_CH)
		return -1;
	int n = REAC_GRANT_SWEEP_LEN(width);
	if (max < n)
		return -1;

	/* THE ORDER, measured on both real M-200 goldens (deduped by frame counter):
	 *
	 *   HEAD_ACK | A[base].0 A[base].1 A[base].2 | HEAD_MARK | B x6 |
	 *   A[base+1].0..2 | A[base+2].0..2 | ... | A[base+w-1].0..2
	 *
	 * i.e. the FIRST allocated channel's three head-amp records ride up front,
	 * bracketed by the two 0014 head frames, with group B wedged between them and
	 * the rest of group A. 8 + w*3 frames: 32 for an S-0808, 56 for an S-1608 —
	 * frame-for-frame the shape of the tables this replaces.
	 *
	 * ONE-SHOT BURST, not a periodic stream (GRANT-SWEEP.md: "MEASURED SHAPE — do
	 * not 'correct' to a periodic stream"). The caller (reac_master) spaces these
	 * rows one per grant_stride slots and then goes calm.
	 *
	 * NOT x2 REPEATS. The captures show 96 group-A frames for a 16-input box, and
	 * the RE notes read that as "16 x 3 x 2 repeats". It is not: the pairs carry an
	 * IDENTICAL frame counter (bytes 14-15, which a real master increments every
	 * slot) and differ only in captured length (1494 vs 1492 = the +2 FCS). They are
	 * ONE frame seen twice by the switch mirror — the same mirror artifact this repo
	 * already documents for the "1494 B OHRCA frame" (reac_tx.h, #156). Deduped by
	 * counter, both goldens hold exactly 48 unique group-A records, one each. Emitting
	 * each record twice would be reproducing a capture artifact.
	 */
	int k = 0;
	memcpy(sweep[k++], GRANT_HEAD_ACK, 34);
	for (uint8_t p = 0; p < REAC_HEADAMP_NPARAMS; p++)
		if (put_groupa(sweep[k++], base, p, values[p]) != 0)
			return -1;
	memcpy(sweep[k++], GRANT_HEAD_MARK, 34);
	for (int i = 0; i < REAC_GRANT_GROUPB_LEN; i++)
		memcpy(sweep[k++], GRANT_GROUPB[i], 34);
	for (int c = 1; c < width; c++) {
		uint8_t ch = (uint8_t)(base + c);
		for (uint8_t p = 0; p < REAC_HEADAMP_NPARAMS; p++)
			if (put_groupa(sweep[k++], ch, p,
			               values[c * REAC_HEADAMP_NPARAMS + p]) != 0)
				return -1;
	}
	return k;
}

/* ---- head-amp granularity ------------------------------------------------
 * See reac/reac_ctrlblk.h: the wire record is per channel for all three
 * parameters, the slot map is per slot, and the per-four field is the inventory
 * cell. Phantom's shift was disputed and these two functions used to return
 * REAC_HEADAMP_GRAN_DISPUTED for it; the wire settled it at 0 on 2026-08-23.
 * They stay as predicates so no caller open-codes a shift, which is how the
 * wrong one spread in the first place. */
int reac_headamp_group_of(uint8_t ch, uint8_t param)
{
	switch (param) {
	case REAC_HEADAMP_SENS:    return ch >> REAC_HEADAMP_GRAN_SENS_SHIFT;
	case REAC_HEADAMP_PAD:     return ch >> REAC_HEADAMP_GRAN_FLAGS_SHIFT;
	case REAC_HEADAMP_PHANTOM: return ch >> REAC_HEADAMP_GRAN_PHANTOM_SHIFT;
	default:                   return -1;
	}
}

int reac_headamp_record_carries(uint8_t ch, uint8_t param)
{
	switch (param) {
	case REAC_HEADAMP_SENS:
	case REAC_HEADAMP_PAD:
	case REAC_HEADAMP_PHANTOM:
		return 1;                       /* every channel carries all three */
	default:
		return -1;
	}
}
