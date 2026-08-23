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
 * SENS is a step index, and the S-1608 turns it into hardware through a 56-entry
 * table at 0x0c0327a0 in its own image (link base 0x0BFE0000), reached by BOTH
 * write paths — the immediate writer and the stepped updater. 56 entries is
 * exactly the 56 legal SENS values 0x00..0x37; there are no spare rows. (The
 * 0..47 that head-amp code also deals in is the CHANNEL space, 16 channels at
 * base 0x20 — a different axis entirely.)
 *
 * Each entry is a (coarse, fine) pair, not a dB value: four coarse stages with
 * sixteen fine steps of 2 each (the first stage has only eight), so the table
 * says WHERE the curve breaks and the metal has to say by how much. The breaks
 * are at indices 8, 24 and 40, and the table ends exactly where the image's
 * "V03.05" version string begins.
 *
 * MEASURED on an S-0808, using the preamp's own NOISE FLOOR — which tracks gain
 * exactly inside a stage, where the noise figure is constant, and which
 * reproduces to 0.03 dB across sessions where a microphone in a room does not:
 *
 *   stage 2 (idx  8..23)   0.90 dB per step
 *   stage 1 (idx 24..39)   0.95 dB per step
 *   stage 0 (idx 40..55)   0.98 dB per step
 *   stage 3 (idx  0.. 7)   UNMEASURED — its floor sits under the converter's,
 *                          so 0.90 is carried over from stage 2 and is a guess
 *
 * ACROSS a break the floor is NOT a gain probe, because the noise figure changes
 * there too: at 23->24 the floor drops 6.06 dB while the SIGNAL is flat to within
 * the source's own spread. So gain is continuous across the breaks — the drop is
 * the preamp switching to a quieter input stage — and the table below carries no
 * step at 8, 24 or 40.
 *
 * TOTAL SPAN 48.75 dB, not the 55 dB a flat 1 dB per step implies.
 *
 * THE MAP IS NOT INJECTIVE. Because gain is continuous across the three breaks,
 * steps 7 and 8, 23 and 24, and 39 and 40 deliver the SAME gain. They differ in
 * noise: the upper twin is in the quieter stage (measured 6.06 dB lower floor at
 * 24 than at 23), so it is strictly better and the reverse lookup returns it. A
 * round trip is therefore the identity everywhere except those three lower twins,
 * which it promotes to their quieter partner — deliberately, and asserted.
 *
 * A NOTE ON UNITS, because it is a real limitation and not a detail. The steps
 * are all under 1 dB, so an integer-dB API cannot represent them: neighbouring
 * steps collide on the same integer and step -> dB -> step cannot be the
 * identity. The centi-dB entry points below are exact and round-trip; the
 * integer-dB pair is kept for callers that still speak whole dB and is
 * documented as lossy rather than quietly wrong. */
#define REAC_HEADAMP_SENS_MAX 0x37   /* 55 — the 56th and last table entry */

/* Sensitivity for a step, in HUNDREDTHS of a dBu. Exact; the inverse round-trips.
 * `pad_on` adds the pad's 20 dB. */
int      reac_headamp_sens_cdb(uint8_t value, int pad_on);
uint8_t  reac_headamp_sens_value_cdb(int cdb, int pad_on);

/* The same in whole dBu. LOSSY BY CONSTRUCTION — the device's steps are smaller
 * than 1 dB, so this cannot round-trip and must not be used to store a setting. */
int      reac_headamp_sens_db(uint8_t value, int pad_on);
uint8_t  reac_headamp_sens_value(int db, int pad_on);


/* ---- frame parsing and the device matrix ---------------------------------
 * Moved out of the daemon because every REAC implementation needs exactly this
 * and would otherwise rewrite it: what a frame IS, and which real box is on the
 * wire. Neither encodes a choice. What stays behind is what does — which slots we
 * grant a box, when we advance a state machine, how we wire it into a graph. */

enum reac_ctrl_kind {
	REAC_CTRL_NONE = 0,      /* not a 0x8819 frame */
	REAC_CTRL_FILLER,        /* type 00 00 (audio/idle), checksum-exempt */
	REAC_CTRL_PROBE,         /* master cdea 01, sub-state cycling (hunting) */
	REAC_CTRL_MASTER_HB,     /* master cdea 01 03 0019 (established heartbeat) */
	REAC_CTRL_MASTER_ANNOUNCE,/* master cfea (announce) */
	REAC_CTRL_GRANT,         /* master cdea 04 03, record TAG 01 00 (the JOIN
	                          * grant-burst; also any 04 03 tag we don't know) */
	REAC_CTRL_HEADAMP,       /* master cdea 04 03, record TAG 01 01 (head-amp:
	                          * CH PARAM VALUE — a preamp knob, NOT a grant) */
	REAC_CTRL_BOX_HB,        /* a box cdea 01 03 0001 81 (our keep-alive) */
	REAC_CTRL_SPLIT_ANNOUNCE,/* a splitter's ceea announce — the split role's
	                          * own frame type (reac-aes67 REAC-PROTOCOL.md §6,
	                          * source-derived; never yet captured, §14.1) */
	REAC_CTRL_UNKNOWN_CTRL,  /* cdea/cfea we don't classify */
};


struct reac_ctrl_parsed {
	enum reac_ctrl_kind kind;
	uint8_t  src[6];
	uint8_t  dst[6];
	int      is_broadcast;   /* dst == ff:ff:ff:ff:ff:ff */
	uint16_t counter;        /* bytes 14-15 LE */
	uint8_t  op0, op1;       /* control opcode bytes [18],[19] */
	uint16_t op_len;         /* BE length [20:22] */
	uint8_t  sel;            /* selector [22] (0x81/0x82/... or a channel byte) */
	uint8_t  sel2;           /* second selector byte [23] (cold-connect: 0x02) */
	uint8_t  ch;             /* HEADAMP only: wire channel (model_base + input-1) */
	uint8_t  param;          /* HEADAMP only: enum reac_headamp_param */
	uint8_t  value;          /* HEADAMP only: 0|1 (phantom/pad) or 0x00..0x37 (SENS) */
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
	REAC_HEADAMP_SENS    = 0x02,   /* sensitivity (value 0x00..0x37, 1 dB/step) */
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
/* Three head-amp parameters per channel. A protocol bound, so it lives with the
 * records that carry it rather than in one caller's header.
 * (REAC_HEADAMP_MAX_CH is defined once, above, with the head-amp CH space.) */
#define REAC_HEADAMP_NPARAMS    3

#define REAC_GRANT_GROUPB_LEN   6
#define REAC_GRANT_SWEEP_LEN(w) (8 + (w) * 3)

int reac_ctrl_build_grant_sweep(uint8_t sweep[][34], int max, uint8_t base,
                                int width, const uint8_t *values);

#endif /* REAC_CTRLBLK_H */
