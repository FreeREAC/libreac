// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_ctrlblk — the 32-byte REAC control block, and the scene transfer on it.
 *
 * This is protocol, not policy: the checksum rules every REAC control frame
 * obeys, and the master's scene push built on them. Any REAC implementation
 * needs exactly this and would otherwise reimplement it, which is why it lives
 * in the library rather than in one daemon. What stays OUT is everything that
 * encodes a choice: establishment policy, slot allocation, transport, node
 * wiring. Those differ per implementation; these bytes do not.
 *
 * LICENCE: GPL-3.0-or-later, matching the rest of libreac. A kernel-side REAC
 * would need this layer — the kernel is GPL-2.0-ONLY, so GPL-3 code cannot link
 * into it — and the project's answer is a carve-out limited to the files a module
 * actually needs, not a blanket relicence. That list was scoped before this file
 * existed and does not name it. It is therefore a CANDIDATE for the carve-out and
 * is written to be eligible; whoever designs the mechanism should decide, and
 * nothing here presumes the outcome.
 *
 * KERNEL-PORTABLE BY CONSTRUCTION, which is what keeps that option open. These
 * are commitments, not style:
 *   - no allocation, here or anywhere below;
 *   - no floating point;
 *   - buffers are always the CALLER's, with an explicit length;
 *   - no sockets, files, threads or globals owned internally;
 *   - failure is a return value, never an exit or a log;
 *   - freestanding-friendly: stdint/stddef and memcpy/memset only.
 * A contributor who breaks one of these closes the door this licence opened.
 */
#ifndef REAC_CTRLBLK_H
#define REAC_CTRLBLK_H

#include <stdint.h>
#include <stddef.h>

/* ---- the control block ----------------------------------------------------
 * A REAC frame carries a 32-byte control block at [18:50]; the last byte of it
 * is a checksum. FILLER frames (type 0x0000) carry audio there instead and are
 * checksum-exempt. */
#define REAC_CTRL_BLOCK_OFF   18   /* control block / checksum region start */
#define REAC_CTRL_BLOCK_END   50   /* one past end (= audio offset)         */
#define REAC_CTRL_CKSUM_OFF   49   /* checksum byte (last of the block)     */
#define REAC_CTRL_BLOCK_LEN   32   /* the checksummed block, [18:50]        */

/* Set the block's last byte so the 32 bytes sum to 0 mod 256. */
void reac_ctrl_block_cksum_stamp(uint8_t block[REAC_CTRL_BLOCK_LEN]);

/* The same rule applied to a whole frame's [18:50], and its verifier. */
void reac_ctrl_checksum_apply(uint8_t *frame);
int  reac_ctrl_checksum_verify(const uint8_t *frame);

/* The NESTED record checksum (head-amp DT1 and friends): a record's last byte is
 * set so the record sums to 0x80, not to 0. It is stamped BEFORE the block
 * checksum that encloses it — a record fixed up afterwards invalidates the
 * block, which is a real bug this ordering exists to prevent. */
void reac_ctrl_record_cksum_stamp(uint8_t *rec, size_t n);
int  reac_ctrl_record_cksum_verify(const uint8_t *rec, size_t n);

/* ---- THE CONTROL BLOCK'S HEADER: FOUR FIELDS, NOT AN OPCODE ---------------
 *
 * Every builder in both stagebox images lays down the same four fields before
 * anything else (S-1608 FUN_0c003398, the box's own segmented upload):
 *
 *   block[0]    u8      LINK      which link the message belongs to
 *   block[1]    u8      SEGMENT   bit 0 FIRST, bit 1 LAST
 *   block[2:4]  u16 BE  LENGTH    always a length, never a selector
 *   block[4]    u8      OPCODE    what the message is
 *   block[31]   u8      checksum  -Sum(block[0..30])
 *
 * SO THERE ARE NOT SEVEN SCENE-AND-RECORD OPS. What the wire notes used to call
 * ops 0x0101 / 0x0100 / 0x0102 / 0x0103 are ONE transfer on link 1 reading
 * FIRST / MIDDLE / LAST / SINGLE, and 0x0401 / 0x0402 / 0x0403 are the same
 * three states on link 4. Reading the first two bytes as a 16-bit opcode models
 * a field that is not there, and it is why a control block used to be told apart
 * by its LENGTH: the discriminator is block[4], as the box's own receive
 * dispatch uses it (its state-2 gate is buf[0]==1 && (buf[1] & 1) && buf[4]==0).
 *
 * block[2:4] is a length in every case. What varies is its base: for the bulk
 * opcode on link 1 it counts this frame's payload bytes only, and everywhere
 * else it counts from block[4] inclusive. Each sub-page does happen to have a
 * distinct length, so switching on it appears to work — right up to a window
 * that is not full.
 */
#define REAC_LINK_CTRL     1   /* the stagebox control link                    */
#define REAC_LINK_SECOND   2   /* an S-4000S-only second link                  */
#define REAC_LINK_RECORD   4   /* the record link — DT1 containers             */

#define REAC_SEG_MIDDLE 0x00   /* neither bit: a continuation                  */
#define REAC_SEG_FIRST  0x01   /* bit 0                                        */
#define REAC_SEG_LAST   0x02   /* bit 1                                        */
#define REAC_SEG_SINGLE 0x03   /* both: a complete message in one frame        */

/* Link-1 opcodes, block[4]. */
#define REAC_OP_BULK       0x00  /* the scene push, in all four segment states */
#define REAC_OP_SLOT_MAP   0x01  /* the slot-record window; also the poll a box
                                  * answers with REAC_OP_BOX_HB               */
#define REAC_OP_GROUP_MAP  0x10  /* ten bytes of packed group fields          */
#define REAC_OP_DECL_ALT   0x80  /* the box's declaration, alternate arm       */
#define REAC_OP_BOX_HB     0x81  /* the box's heartbeat reply, opcode only     */
#define REAC_OP_DECL       0x82  /* the box's declaration                      */
#define REAC_OP_DECL_OTHER 0x84  /* the same message with the other constant   */

/* The Roland DT1 record a link-4 container carries: block[14] is the low byte of
 * the model id, block[15] the command, block[16:18] the register page (the TAG),
 * and the record the two checksums enclose runs block[16..21]. Frame-relative
 * that is [32], [33], [34:36] and [34..39]. */
#define REAC_DT1_MODEL_LO  0x12
#define REAC_DT1_CMD_RQ1   0x11  /* a request                                  */
#define REAC_DT1_CMD_DT1   0x12  /* a set                                      */
#define REAC_DT1_TAG_HEAD_MARK 0x0000
#define REAC_DT1_TAG_JOIN      0x0100
#define REAC_DT1_TAG_HEADAMP   0x0101
#define REAC_DT1_TAG_BOX_READY 0x0302
#define REAC_DT1_TAG_IDENTITY  0x0500

/* ---- THE SCENE PUSH: the master's enrolment transfer -----------------------
 *
 * After link-up a desk pushes its scene to the box as one bounded transfer:
 *
 *   op-0101 header  — declares the TOTAL (0x22c8) and carries the body's first 24 B
 *   op-0100 chunk   — 26 B of body, x341
 *   op-0102 final   — the last 14 B
 *
 *   24 + 341*26 + 14 = 8904 = 0x22c8
 *
 * The S-1608 accepts the header only when the declared total equals 0x22c8 (the
 * same constant resolved out of its own firmware image), and it completes
 * reassembly ONLY on the final chunk. Completion runs its state-4 COMMIT — the
 * sole promoter of staged head-amp into the active table. A transfer that stops
 * short leaves the box in reassembly for the life of the link, so every head-amp
 * record it receives is written to STAGING and never promoted. That is why
 * byte-perfect head-amp records never lit a 48 V LED.
 *
 * Measured on a real M-200i driving an S-1608: the 341 chunks go out back-to-back
 * in 0.680 s, the box answers 01 03 0010 once the transfer completes, and only
 * then does the desk open its grant window. */
#define REAC_SCENE_BYTES        8904   /* == 0x22c8, the declared total          */
#define REAC_SCENE_HEAD_BYTES     24   /* body bytes carried by the op-0101      */
#define REAC_SCENE_CHUNK_BYTES    26   /* body bytes per op-0100 (its 0x001a)    */
#define REAC_SCENE_TAIL_BYTES     14   /* body bytes carried by the op-0102      */
#define REAC_SCENE_CHUNKS        341   /* op-0100 count for a whole body         */
#define REAC_SCENE_STEPS   (1 + REAC_SCENE_CHUNKS + 1)

/* WHAT THE BOX VALIDATES. The state-4 commit does three four-byte compares and
 * promotes NOTHING if any one misses, while the transfer still looks complete
 * from outside. "1234" rides the header; SYSP and SCEN ride op-0100 chunks 32
 * and 33, so a body whose MIDDLE chunks are wrong fails silently. */
#define REAC_SCENE_TAG_ID_OFF    0x000   /* "1234" — rides the op-0101 header  */
#define REAC_SCENE_TAG_SYSP_OFF  0x368   /* "SYSP" — rides op-0100 chunk 32    */
#define REAC_SCENE_TAG_SCEN_OFF  0x37c   /* "SCEN" — rides op-0100 chunk 33    */

/* The master's own MAC sits INSIDE the body. On-wire identity must equal the L2
 * source, so a master replaying a recovered body substitutes its own. */
#define REAC_SCENE_MAC_OFF       0x340   /* = 832 */

/* Build one step of the transfer into a 34-byte [type|block] template (the shape
 * a master stamps into frame [16:50]), checksum applied. `n` must be
 * REAC_SCENE_BYTES. Returns 0, or -1 on a bad step index or a partial body. */
int reac_ctrl_build_scene_step(uint8_t blk[34], const uint8_t *body, size_t n,
                               int step);

/* Substitute `mac` into a mutable body at REAC_SCENE_MAC_OFF. 0, or -1. */
int reac_ctrl_scene_set_mac(uint8_t *body, size_t n, const uint8_t mac[6]);

/* Build a body from zeros, the three validated tags and `mac`.
 *
 * REFUTED AS A DRIVER, 2026-08-23, and kept because the refutation is the useful
 * part: a tag-only body passes the commit but leaves a real S-1608 reporting
 * model=unknown with ZERO capture ports, where a recovered body brings it up as
 * s1608 with 16. The box reads far more of this body than the commit validates —
 * its own configuration comes out of here too. The firmware sweep that suggested
 * otherwise zeroed one 128-byte window AT A TIME, which shows each window is
 * individually unvalidated BY THE COMMIT and nothing more. Generating a usable
 * scene means reproducing that structure. Returns 0, or -1. */
int reac_ctrl_scene_build(uint8_t *body, size_t n, const uint8_t mac[6]);

/* ---- HEAD-AMP SENSITIVITY: the box's own step -> gain curve ----------------
 *
 * ONE DECIBEL PER STEP, all 56 of them, with no duplicate steps anywhere.
 * Sensitivity runs -10 dBu at 0x00 down to -65 dBu at 0x37, pad off; the pad
 * shifts the whole travel up by 20. The curve is declared once in
 * reac-protocol's spec/protocol-facts.yaml (group `headamp_sens`) and this is
 * its C spelling.
 *
 * MEASURED 2026-08-23 on an S-0808, all 56 steps, output 1 cabled to input 1 so
 * the source is an electrical loopback of a level we generated and therefore
 * know. Raw data in reac-pw docs/measurements/sens-sweep-2026-08-23-*.csv:
 *
 *   span 0x00 -> 0x37     54.60 dB          (a flat 1 dB/step implies 55.00)
 *   least-squares slope    0.988 dB/step     max residual 0.44 dB over 55 steps
 *   pad, measured          20.12 and 20.20 dB at two different steps
 *
 * The residual is the size of the measurement's own scatter, so the law this
 * spells is the round 1 dB and not the fitted 0.988.
 *
 * WHAT THIS REPLACES, because it was here and it was wrong. A 56-entry table
 * read out of the S-1608's image at 0x0c0327a0 gives four coarse stages with
 * breaks at 8, 24 and 40. That structure is real. What was inferred from it was
 * not: that gain is CONTINUOUS across a break, so 7/8, 23/24 and 39/40 deliver
 * identical gain and the map is not injective. All three pairs were put to a
 * rapid A/B/A alternation, twice each, at two generator levels:
 *
 *    7 -> 8    +0.92 dB and +1.12 dB     (drift control: 0.08 / 0.10 dB)
 *   23 -> 24   +1.36 dB and +1.31 dB     (drift control: 0.34 / 0.15 dB)
 *   39 -> 40   +0.97 dB and +0.84 dB     (drift control: 0.26 / 0.08 dB)
 *
 * Every pair steps by about a decibel, an order of magnitude outside its own
 * control. There are no twins, so the map IS injective and a round trip is the
 * identity everywhere.
 *
 * WHY THE EARLIER NUMBERS CAME OUT LOW (0.90 / 0.95 / 0.98 per step, span
 * 48.75): they were taken from the preamp's own NOISE FLOOR. A floor is not a
 * gain probe. What it measures is gain x input-referred noise PLUS whatever the
 * output stage and converter add after the gain, and that second term does not
 * scale — so the floor's slope is always shallower than the gain's, and most so
 * at low gain. It is a good probe of REPEATABILITY (0.03 dB across sessions) and
 * a biased probe of SLOPE. The 6.06 dB floor drop at 23->24 is real and stands;
 * it is the preamp switching to a quieter input stage, and it is a noise-figure
 * step sitting on top of an ordinary 1 dB gain step, not instead of one.
 *
 * A NOTE ON UNITS. The step is a whole decibel, so the integer-dB pair below is
 * exact and round-trips; the centi-dB pair is kept because it is the published
 * entry point and because a future finding of sub-dB steps would land in it
 * without moving every caller. */
#define REAC_HEADAMP_SENS_MAX 0x37   /* 55 — the 56th and last step */

/* The curve itself, in HUNDREDTHS of a dBu, public so the schema can bind to it:
 * protocol-facts.yaml's `headamp_sens` group names these three and the generated
 * reac_facts_assert.h refuses to compile if either side moves alone. */
#define REAC_HEADAMP_SENS_REF_CDB  (-1000)   /* step 0x00, pad off: -10.00 dBu */
#define REAC_HEADAMP_SENS_STEP_CDB   (100)   /* 1.00 dB, every step */
#define REAC_HEADAMP_PAD_CDB        (2000)   /* the pad's 20.00 dB */

/* Sensitivity for a step, in HUNDREDTHS of a dBu. Exact; the inverse round-trips.
 * `pad_on` adds the pad's 20 dB. */
int      reac_headamp_sens_cdb(uint8_t value, int pad_on);
uint8_t  reac_headamp_sens_value_cdb(int cdb, int pad_on);

/* The same in whole dBu. Also exact, and also round-trips: the device's step is
 * one whole decibel. */
int      reac_headamp_sens_db(uint8_t value, int pad_on);
uint8_t  reac_headamp_sens_value(int db, int pad_on);


/* ---- frame parsing and the device matrix ---------------------------------
 * Moved out of the daemon because every REAC implementation needs exactly this
 * and would otherwise rewrite it: what a frame IS, and which real box is on the
 * wire. Neither encodes a choice. What stays behind is what does — which slots we
 * grant a box, when we advance a state machine, how we wire it into a graph. */

/* WHAT A CONTROL FRAME IS, decided the way the box decides it: the link at
 * block[0], the segment bits at block[1] and the opcode at block[4]. Never the
 * length — see the header section above for why a length looks like it works. */
enum reac_ctrl_kind {
	REAC_CTRL_NONE = 0,       /* not a 0x8819 frame                            */
	REAC_CTRL_FILLER,         /* type 00 00 (audio/idle), checksum-exempt      */
	REAC_CTRL_SCENE_TRANSFER, /* link 1, opcode 0x00 — the master's scene push,
	                           * in any of its four segment states             */
	REAC_CTRL_MASTER_HB,      /* link 1, opcode 0x01 — the slot-record window,
	                           * which is also the poll a box answers          */
	REAC_CTRL_MASTER_ANNOUNCE,/* master cfea (announce)                        */
	REAC_CTRL_GRANT,          /* link 4, SINGLE, DT1 tag != head-amp: the join
	                           * grant and the cold-connect inventory tags     */
	REAC_CTRL_HEADAMP,        /* link 4, SINGLE, DT1 tag 0x0101 — CH PARAM
	                           * VALUE, a preamp knob, NOT a grant             */
	REAC_CTRL_BOX_HB,         /* link 1, opcode 0x81 — the box's reply         */
	REAC_CTRL_SPLIT_ANNOUNCE, /* a splitter's ceea announce — the split role's
	                           * own frame type (reac-aes67 REAC-PROTOCOL.md §6,
	                           * source-derived; never yet captured, §14.1)    */
	REAC_CTRL_CONFIG_ANNOUNCE,/* link 1, opcode 0x80/0x82/0x84 — the box's own
	                           * declaration, and a SYMMETRIC exchange: the box
	                           * image builds exactly the message it parses    */
	REAC_CTRL_GROUP_MAP,      /* link 1, opcode 0x10 — ten bytes of packed
	                           * group fields. Only the S-4000S image has a
	                           * handler; the S-1608 has none at all           */
	REAC_CTRL_RECORD_FRAGMENT,/* link 4, FIRST or LAST — HALF of a DT1 record.
	                           * The two fragment bodies concatenate into one
	                           * SysEx whose inner checksum closes only ACROSS
	                           * BOTH, so a fragment on its own is a truncated
	                           * record and its tag, fields and checksum must
	                           * not be read as if it were whole              */
	REAC_CTRL_LINK2,          /* link 2 — present in the S-4000S image, which
	                           * length-checks it, routes three subtypes and
	                           * throws all three away: empty stubs in ver2200 */
	REAC_CTRL_UNKNOWN_CTRL,   /* cdea/cfea we do not classify                  */
};

/* The kind's name, for logs and for a report that has to stay diffable. */
const char *reac_ctrl_kind_name(enum reac_ctrl_kind kind);

struct reac_ctrl_parsed {
	enum reac_ctrl_kind kind;
	uint8_t  src[6];
	uint8_t  dst[6];
	int      is_broadcast;   /* dst == ff:ff:ff:ff:ff:ff */
	uint16_t counter;        /* bytes 14-15 LE */

	/* The header, field for field. There is no 16-bit "op": a caller that wants
	 * the old 0x0103 spelling is asking for two different fields glued. */
	uint8_t  link;           /* block[0]   — REAC_LINK_*                        */
	uint8_t  seg;            /* block[1]   — REAC_SEG_* bits                    */
	uint16_t blk_len;        /* block[2:4] — BE, always a length                */
	uint8_t  opcode;         /* block[4]   — the discriminator                  */

	/* Link 4 only, and only on a SINGLE: a fragment carries half a record. */
	uint16_t dt1_tag;        /* block[16:18] = frame[34:36]                     */
	uint8_t  ch;             /* HEADAMP: wire channel (model_base + input-1)    */
	uint8_t  param;          /* HEADAMP: enum reac_headamp_param                */
	uint8_t  value;          /* HEADAMP: 0|1 (phantom/pad) or 0x00..0x37 (SENS) */
};


/* Classify a frame. Returns the kind and fills *out (which may be NULL). */
enum reac_ctrl_kind reac_ctrl_parse(const uint8_t *frame, size_t len,
                                    struct reac_ctrl_parsed *out);

/* ---- FIXED box-model matrix ----
 * A REAC stagebox is identified on the wire by three orthogonal fields (see
 * docs/REAC-BOX-STATE-DIAGRAM.md): the config-announce SELECTOR byte (model
 * family), an optional ASCII NAME frame (exact model within the 0x84 family),
 * and the channel DESCRIPTOR + width (52 + 36*in_ch bytes). We ship a fixed
 * table of byte-verified real models so a model always matches its channels —
 * there is no "S-1608 with 8 channels". Pick a row by token or by in-channel
 * count; both resolve to the same entry. */
struct reac_box_model {
	const char *token;      /* CLI token: "s1608", "s0808"              */
	const char *display;    /* human label for --help / logs            */
	int         in_ch;      /* box input (upstream) width -> frame size  */
	int         out_ch;     /* box output (downstream) width             */
	uint8_t     config_block[32];  /* config-announce cdea 01 03 0010    */
	int         has_name;   /* 1 -> also emit the ASCII name frame       */
	uint8_t     name_block[32];    /* name frame cdea 04 01 001b (if any)*/
	/* The mixer identifies the MODEL from the cold-connect INVENTORY frames, not
	 * just the config-announce: the 0016/001a blocks differ per model, and some
	 * models emit an extra 0402000d frame. Byte-verified per model. */
	uint8_t     cc0014[32];        /* cold-connect cdea 04 03 0014       */
	uint8_t     cc0013[32];        /* cold-connect cdea 04 03 0013       */
	uint8_t     cc0016[32];        /* cold-connect cdea 04 03 0016       */
	uint8_t     cc001a[32];        /* cold-connect cdea 04 03 001a       */
	int         has_extra;  /* 1 -> also emit the cdea 04 02 000d frame  */
	uint8_t     extra_block[32];   /* cdea 04 02 000d (if any)           */
};
const struct reac_box_model *reac_box_model_by_token(const char *token);
const struct reac_box_model *reac_box_model_by_channels(int in_ch);
const struct reac_box_model *reac_box_model_table(size_t *count);

/* The box a cold-connect/config frame belongs to, or NULL when no row matches.
 * Never guesses: an unknown width is unknown, not the nearest model. */
const struct reac_box_model *reac_ctrl_identify_box(const uint8_t *frame, size_t len);

/* The wire length of a box->master frame at a given input width. */
size_t reac_ctrl_box_frame_len(int n_ch);

/* Builders. All emit unicast-to-master (dst=master), our Roland-OUI src, the
 * given free-running u16-LE counter, EtherType 0x8819, checksum applied.
 * Return the frame length in bytes, or 0 on bad args.
 *
 * GROUND-TRUTHED (byte-compared to captured box frames): */
size_t reac_ctrl_build_box_hb(uint8_t *out, const uint8_t master[6],
                              const uint8_t src[6], uint16_t counter, int n_ch);
/* upstream return audio: n_ch x 12 samples, planar float [ch][s]; box-width
 * frame (16ch->628B, 8ch->340B): 18 hdr + 32 descriptor + n_ch*36 audio + 2 tail. */
size_t reac_ctrl_build_upstream_filler(uint8_t *out, const uint8_t master[6],
                                       const uint8_t src[6], uint16_t counter,
                                       int n_ch, float *const *planar, int ns);

/* The presence-flood FILLER (broadcast, unlinked): zero control block [18:50] (no
 * 0x7a descriptor) over LIVE audio [50:626] — what a real box broadcast-floods to
 * announce presence on a cold boot. Audio is planar float [ch][s], as
 * build_upstream_filler; NULL planar -> silent. */
size_t reac_ctrl_build_flood_filler(uint8_t *out, const uint8_t bcast[6],
                                    const uint8_t src[6], uint16_t counter,
                                    int n_ch, float *const *planar, int ns);



/* MASTER-side box RECOGNITION (the mirror of the slave emitter): given a raw
 * received frame, if it is a box config-announce (cdea 01 03 0010) whose
 * descriptor block matches a fixed-matrix row, return that model; else NULL.
 * "The matrix is law as a stagebox; as a mixer we read the frame and use the
 * matrix as the default" — a NULL means no known model, and the caller falls
 * back to the descriptor/width carried in the frame. PURE (no socket). */

/* The RETIRED --box pin's one and only remaining job: say ONCE that what somebody
 * typed disagrees with what the wire declared. `*pin` is the raw pin value
 * ("s1608" or "s1608:Drums"; NULL = nothing was typed) and is CONSUMED — set to
 * NULL — on the first call that sees a recognized model, whether or not it
 * disagreed. Returns 1 exactly when a notice is due.
 *
 * "Exactly once" is the whole contract, and it is why this is a function and not
 * three lines at a call site: the box repeats its config-announce, so a notice
 * evaluated per frame becomes several thousand identical lines an hour and stops
 * being read. Saying nothing is the other failure — that is how a wrong pin sits
 * in reac.env for a session. PURE: no I/O, the caller does the printing. */
int reac_box_pin_notice(const char **pin, const char *recognized_token);

/* Config-announce (cdea 01 03 0010) — the SETUP DECLARATION the master enrolls
 * the box from. Byte-verified per model; the selector byte sets the displayed
 * model family. in_ch selects the fixed-matrix row (falls back to S-1608). */
size_t reac_ctrl_build_config_announce(uint8_t *out, const uint8_t master[6],
                                       const uint8_t src[6], uint16_t counter, int in_ch);
/* ASCII model-name frame (cdea 04 01 001b) — required for the 0x84 family so the
 * desk shows the exact model (e.g. "S-0808") instead of the generic family name.
 * Returns 0 (emits nothing) for models whose name comes from the selector alone
 * (the 0x82 / S-1608 family). */
size_t reac_ctrl_build_name_frame(uint8_t *out, const uint8_t master[6],
                                  const uint8_t src[6], uint16_t counter, int in_ch);
/* The extra cold-connect frame (cdea 04 02 000d) some models send (S-0808). The
 * mixer uses it, with the 0016/001a inventory, to determine the exact model.
 * Returns 0 (emits nothing) for models that don't send it (e.g. S-1608). */
size_t reac_ctrl_build_extra_frame(uint8_t *out, const uint8_t master[6],
                                   const uint8_t src[6], uint16_t counter, int in_ch);
/* The box cold-connect (cdea 04 03): the 32-byte control block over LIVE audio
 * [50:626] (the [38:66] region is per-frame audio, NOT device inventory). Audio is
 * planar float [ch][s], as build_upstream_filler; NULL planar -> silent. The master
 * echoes the control block verbatim as its grant. */
size_t reac_ctrl_build_coldconnect(uint8_t *out, const uint8_t master[6],
                                   const uint8_t src[6], uint16_t counter,
                                   int n_ch, float *const *planar, int ns);

/* The cdea 04 03 0013 cold-connect variant, interleaved with the 0014 by a real
 * box. Emitted raw (the 0013 block is not sum-to-0). */
size_t reac_ctrl_build_coldconnect_0013(uint8_t *out, const uint8_t master[6],
                                        const uint8_t src[6], uint16_t counter,
                                        int n_ch, float *const *planar, int ns);

/* The cdea 04 03 0016 and 001a cold-connect variants — the rest of the escalation
 * a real S-1608 sends (0014 -> 0013 -> 0016 -> 001a). They carry the fuller box
 * inventory the master needs to register the box in its REAC device list. Emitted
 * raw (byte-matched to a real S-1608, 2026-07-11). */
size_t reac_ctrl_build_coldconnect_0016(uint8_t *out, const uint8_t master[6],
                                        const uint8_t src[6], uint16_t counter,
                                        int n_ch, float *const *planar, int ns);
size_t reac_ctrl_build_coldconnect_001a(uint8_t *out, const uint8_t master[6],
                                        const uint8_t src[6], uint16_t counter,
                                        int n_ch, float *const *planar, int ns);

/* ---- Head-amp source control (op 04 03, record TAG 01 01) ----
 * Ground-truthed on a live M-200 driving an S-0808 + S-1608 (reac-captures/
 * m200-headamp-re/DECODE.md, 2026-07-17): op 04 03 is a RECORD CONTAINER, and
 * the record after the 12 12 marker is TAG(2) DATA(n) CKSUM(1). TAG 01 01 is
 * the console's preamp command, DATA = CH PARAM VALUE. CH is the WIRE channel:
 * model_base + (box_input - 1), model_base S-0808/S-4000S 0x00, S-1608 0x20.
 * Two nested checksums: the record TAG..CKSUM sums to 0x80 mod 256, and the
 * enclosing 32-byte block keeps the usual sum-to-0 at [49]. */
enum reac_headamp_param {
	REAC_HEADAMP_PHANTOM = 0x00,   /* +48V on/off (value 0|1) */
	REAC_HEADAMP_PAD     = 0x01,   /* -20 dB pad on/off (value 0|1) */
	REAC_HEADAMP_SENS    = 0x02,   /* sensitivity step, 0x00..0x37, a flat 1 dB
	                                * each — see reac_headamp_sens_cdb and
	                                * REAC_HEADAMP_SENS_STEP_CDB */
};

/* The head-amp WIRE-CHANNEL space: 0x00..0x2f, so 0x30 = 48 addressable channels.
 *
 * This is NOT libreac's REAC_MAX_CHANNELS (40). The two are DIFFERENT spaces and
 * conflating them is a bug (fixed 2026-07-17): REAC_MAX_CHANNELS is the count of
 * AUDIO slots carried in a downstream frame (REAC_AUDIO_FABRIC_SLOTS), whereas a
 * head-amp record's CH is a wire channel = model_base + (box_input - 1), running to
 * the 0x2f ceiling (the same ceiling reac_master.c's chanmap ring already encodes as
 * REAC_M_CHANMAP_RING = 48 channels + the 0xfe marker). A 16-input S-1608 based at
 * 0x20 occupies 0x20..0x2f = 32..47, so a table bounded by 40 silently REJECTED
 * that box's inputs 9..16 — its top half could never be given phantom/pad/sens.
 * This is the head-amp one, and it is defined HERE and only here. It used to be
 * spelled REAC_HEADAMP_SLOTS, which is reac-pw's name for it in reac_slots.h — a
 * header libreac does not have. Carrying that spelling across in the migration
 * left this expanding to an undefined identifier, and a second definition further
 * down agreed on the value, so nothing that compared VALUES could see it. The
 * damage was structural: every translation unit including this header warned, and
 * a -Werror consumer could not build at all. One name, one definition, here. */
#define REAC_HEADAMP_MAX_CH 48

/* Build the head-amp command frame (master->box direction, downstream width:
 * a real console BROADCASTS these interleaved in its stream — pass the
 * broadcast MAC as the dst like every builder's first MAC arg). ch is the
 * already-resolved wire channel. Returns the frame length, or 0 on a bad
 * param/value combination. */
size_t reac_ctrl_build_headamp(uint8_t *out, const uint8_t master[6],
                               const uint8_t src[6], uint16_t counter,
                               uint8_t ch, uint8_t param, uint8_t value);

/* Stamp a head-amp record over the type [16:18] + control block [18:50] of an
 * ALREADY-BUILT downstream frame, preserving its audio [50:], counter and tail.
 * The MASTER emit path (reac_headamp_tx + the pacer) uses this to overlay a
 * head-amp command onto a FILLER slot without rebuilding the frame. Returns 0, or
 * -1 on a bad param/value (the frame is left untouched). */
int reac_ctrl_stamp_headamp(uint8_t *frame, uint8_t ch, uint8_t param, uint8_t value);

/* Human-readable head-amp parameter name ("phantom" / "pad" / "SENS", or "?"
 * for an unknown param) for logging a received or emitted record. */
const char *reac_headamp_param_name(uint8_t param);

/* Verify a HEADAMP record's INNER checksum. The record bytes TAG..CKSUM (frame
 * offsets [34..39], i.e. block-relative [16..21]) must sum to 0x80 mod 256. Call
 * BEFORE trusting a parsed head-amp CH/PARAM/VALUE so a corrupted knob record is
 * never surfaced or acted on. The frame must already have parsed as
 * REAC_CTRL_HEADAMP (it reads the fixed record offsets). Returns 0 when valid,
 * -1 when the record checksum is wrong. */
int reac_ctrl_headamp_record_verify(const uint8_t *frame);

/* ---- the grant sweep's SHAPE ---------------------------------------------
 * Which records the enrolment burst carries and in which order. WHICH SLOTS a
 * box is given, and what each one's phantom/pad/SENS should be, are the daemon's
 * decisions: the caller passes the values in, `width * REAC_HEADAMP_NPARAMS` of
 * them, laid out [channel][param] from `base`.
 *
 * Group A is the head-amp push (marker 12 12, TAG 01 01, sub-phase 00/01/02 =
 * phantom/pad/SENS, one record per allocated channel per parameter); group B is
 * the fixed six-record constant (marker 12 11, TAG 05 00). Returns the row count
 * written (REAC_GRANT_SWEEP_LEN(width)), or -1. */
/* ---- WHAT IS PER CHANNEL, WHAT IS PER FOUR, AND WHAT IS DISPUTED ----------
 *
 * "Head-amp granularity" used to be one sentence here, and it had a wire fact
 * and a hardware fact folded together. They are separate questions and only one
 * of them is settled.
 *
 * THE WIRE IS PER CHANNEL, ALL THREE PARAMETERS. A DT1 head-amp record is
 * {CH, PARAM, VALUE} and addresses exactly one channel. Every real desk sweep is
 * one contiguous pass over the box's declared width with all three parameters
 * per channel — 24 records for an S-0808, 48 for an S-1608, 96 for an S-4000S.
 * No desk addresses a bank, splits a sweep or repeats one, across three desk
 * generations and 31 of 47 captures. An emitter that skipped records would stop
 * matching the captures.
 *
 * THE SLOT MAP IS PER SLOT, EVERY SLOT. The box's own per-slot table has stride
 * 10 and carries the value byte and three flag bits for each of 0x00..0x2f. No
 * entry in it is shared between channels.
 *
 * THE PER-FOUR FIELD IS THE INVENTORY CELL, NOT PHANTOM. What divides by four in
 * this protocol is the config-announce port table: twelve cells of four channels
 * spanning the 48-channel fabric, and the slot record's HIGH NIBBLE, which
 * carries that cell's code. It is a declaration of what a group of four physical
 * connectors IS — REAC_PORTS_CH_PER_SLOT, in reac_ports.h — and it is not a
 * head-amp parameter. Reading it as "phantom, 4 ch/group" is how the two got
 * folded together.
 *
 * PHANTOM'S HARDWARE ACTUATION GRANULARITY IS OPEN, and libreac will not answer
 * as if it were not. Two readings are live and neither is retired:
 *
 *   TRACE  ch >> 2, per group of four. An executed trace, and a rig measurement
 *          that read sixteen phantom records to 0x20..0x2f as four doing
 *          anything.
 *   STATIC ch >> 0, per channel. The per-slot table read out of the box image
 *          has an addressable flag for every one of 0x00..0x2f, and the field
 *          that divides by four is the inventory cell above.
 *
 * The two agree on a channel that is a multiple of four and disagree everywhere
 * else, so that is exactly where the API answers and where it refuses. A caller
 * that needs the answer has to close the dispute, not read a constant.
 *
 * The readback nibble is a THIRD axis, per eight, and is named apart because
 * collapsing it into either of the above is how a binding gets this wrong.
 */
#define REAC_HEADAMP_GRAN_SENS_SHIFT     0   /* EVIDENCED */
#define REAC_HEADAMP_GRAN_FLAGS_SHIFT    0   /* EVIDENCED */
#define REAC_HEADAMP_GRAN_READBACK_SHIFT 3   /* EVIDENCED */

/* The two live readings of phantom's actuation granularity. There is
 * deliberately no unqualified REAC_HEADAMP_GRAN_PHANTOM_SHIFT: a caller cannot
 * pick a side by accident, and neither can a sweep. */
#define REAC_HEADAMP_GRAN_PHANTOM_SHIFT_TRACE   2
#define REAC_HEADAMP_GRAN_PHANTOM_SHIFT_STATIC  0

/* Returned where the two readings disagree. Distinct from 0, which would mean
 * "the write lands nowhere" — a claim this library is not entitled to make —
 * and from -1, which means the parameter is not a head-amp parameter at all. */
#define REAC_HEADAMP_GRAN_DISPUTED (-2)

/* The group a channel's PARAM addresses, or REAC_HEADAMP_GRAN_DISPUTED for
 * phantom, or -1 for a param outside the three. */
int reac_headamp_group_of(uint8_t ch, uint8_t param);

/* Does a record addressed to `ch` carry `param` to the hardware?
 *   1                            yes
 *   REAC_HEADAMP_GRAN_DISPUTED   phantom off a group-of-four anchor: the two
 *                                readings disagree and nobody knows
 *   -1                           not a head-amp parameter
 * Never 0. The one call that stops a caller open-coding a shift it has to
 * remember, and stops it believing a per-channel phantom write took effect. */
int reac_headamp_record_carries(uint8_t ch, uint8_t param);

/* Three head-amp parameters per channel. A protocol bound, so it lives with the
 * records that carry it rather than in one caller's header.
 * (REAC_HEADAMP_MAX_CH is defined once, above, with the head-amp CH space.) */
#define REAC_HEADAMP_NPARAMS    3

#define REAC_GRANT_GROUPB_LEN   6
#define REAC_GRANT_SWEEP_LEN(w) (8 + (w) * 3)

int reac_ctrl_build_grant_sweep(uint8_t sweep[][34], int max, uint8_t base,
                                int width, const uint8_t *values);

#endif /* REAC_CTRLBLK_H */
