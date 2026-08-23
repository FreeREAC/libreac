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

#endif /* REAC_CTRLBLK_H */
