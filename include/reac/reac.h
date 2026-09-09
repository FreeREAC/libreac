// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// libreac — shared REAC protocol facts + helpers for the FreeREAC C tools
// (reac-aes67, reac-repacer, ...). See https://github.com/FreeREAC/reac-protocol
// for the wire-format reference these constants come from.

#ifndef LIBREAC_REAC_H
#define LIBREAC_REAC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- REAC wire constants ---- */
#define REAC_ETHERTYPE        0x8819  /* non-IP EtherType */
#define REAC_FRAME_BYTES      1492    /* fixed downstream frame: 50 hdr + 1440 audio + 2 end */
#define REAC_AUDIO_BYTES      1440    /* 40 ch x 12 samples x 3 B (rate-invariant) */
#define REAC_AUDIO_OFFSET     50      /* audio block starts here (= L2 header length) */
#define REAC_L2_HEADER_LEN    50      /* 14 eth + 2 counter + 2 type + 32 control */
#define REAC_MAX_CHANNELS     40
#define REAC_SAMPLES_PER_PKT  12      /* downstream: 12 samples/frame at every rate */
#define REAC_RESOLUTION       3       /* bytes per sample per channel (24-bit) */
#define REAC_HDR_COUNTER_OFF  14      /* u16 little-endian sequence counter */
#define REAC_END_MARKER_0     0xC2    /* last two bytes of a full frame */
#define REAC_END_MARKER_1     0xEA

/* Some captures carry 2 extra bytes AFTER the C2 EA end marker. They are NOT a
 * REAC protocol field: they are the low 16 bits of the frame's own Ethernet FCS
 * (crc32 over the preceding bytes, little-endian), left behind by the capture
 * path. Measured 2026-07-29 over the capture corpus: the identity holds for
 * 100% of frames checked, in BOTH directions and across generations —
 * 61,125/61,125 on S-4000 32-ch 1206 B returns, 54,163/54,163 on a second
 * S-4000 unit, 45/45 on S-0808 342 B returns, and every 1494 B downstream
 * sampled on M-200 (NOT OHRCA), M-5000 and S-0808 rigs. A genuine trailer
 * cannot equal the frame's own FCS 115,000 consecutive times.
 *
 * It is therefore NOT OHRCA-specific: 56 captures carry it and 20 carry none;
 * it appears on non-OHRCA M-200 rigs and is absent from OHRCA ones. The variable
 * is the capture rig — mirroring BOTH RX and TX of a port, so a transiting frame
 * is seen twice (same src MAC, same counter, identical payload), one copy clean
 * and one with the residue. Strip it with reac_frame_clean_len(); never model it
 * as a protocol field and never emit it. */
#define REAC_FRAME_BYTES_OHRCA (REAC_FRAME_BYTES + 2)  /* 1494: a 1492 frame plus FCS residue */

/* UPSTREAM (stagebox -> master) frame geometry — box-width sized:
 *     frame_len = REAC_UPSTREAM_OVERHEAD + n_channels * REAC_UPSTREAM_BYTES_PER_CH
 *     S-1608 -> 16 ch -> 628 B;  S-0808 -> 8 ch -> 340 B;  S-4000 -> 32 ch -> 1204 B
 * (the downstream 1492 B broadcast is the 40-ch solution of the same formula). */
#define REAC_UPSTREAM_OVERHEAD     52  /* 50 B header + 2 B end marker */
#define REAC_UPSTREAM_BYTES_PER_CH 36  /* 12 samples x 3 B */

/* THE GEOMETRY IS THE ROLE. A master's downstream is always the 40-channel
 * solution (1492 B); a stagebox's upstream is its own, smaller, declared width.
 * Frame length therefore decides which side of the protocol a peer is, with
 * nothing to decode and no heuristic.
 *
 * This outranks the control frames: a stagebox switched to master mode
 * broadcasts and classifies as `master` by every control-frame rule while still
 * emitting a box geometry. A master never joins another master, so such a peer
 * is a misconfigured box to report, not a master to follow.
 *
 * Pass a CLEAN length (reac_frame_clean_len() first); an FCS residue reads as
 * non-geometric. */
static inline int reac_frame_is_master_downstream(size_t len)
{
	return len == REAC_FRAME_BYTES;
}

/* The channel width a clean REAC frame carries; 0 if `len` is not a legal
 * geometry. The wire declares the width, so nothing configures or remembers it. */
static inline unsigned reac_frame_channels(size_t len)
{
	if (len < REAC_UPSTREAM_OVERHEAD)
		return 0;
	len -= REAC_UPSTREAM_OVERHEAD;
	if (len % REAC_UPSTREAM_BYTES_PER_CH)
		return 0;
	return (unsigned)(len / REAC_UPSTREAM_BYTES_PER_CH);
}

/* A sample-rate descriptor for the master's DOWNSTREAM broadcast (the program the
 * console sends out). That frame is rate-invariant: always 40 ch x 12 samples x 3 B
 * = 1440 B audio, with the sample rate carried by the PACKET RATE (pps =
 * sample_rate / samples_per_pkt: 3675 / 4000 / 8000 at 44.1 / 48 / 96 kHz).
 *
 * A stagebox's UPSTREAM return is different: it carries the box's own input count
 * (variable per box, a smaller frame) and its channel map is FPGA-scrambled. These
 * descriptors model the downstream broadcast, not the upstream return. */
struct reac_mode {
	int sample_rate;     /* 44100 / 48000 / 96000 */
	int n_channels;      /* 40 — the downstream broadcast width */
	int samples_per_pkt; /* 12 per downstream frame at every rate */
};

extern const struct reac_mode REAC_MODE_44K1; /* {44100, 40, 12} — 3675 pps */
extern const struct reac_mode REAC_MODE_48K;  /* {48000, 40, 12} — 4000 pps */
extern const struct reac_mode REAC_MODE_96K;  /* {96000, 40, 12} — 8000 pps */

/* The descriptor for a sample rate (44100 / 48000 / 96000). Unknown rates fall
 * back to 48 kHz (the safe default for the common operational rates). */
const struct reac_mode *reac_mode_for(int sample_rate);

/* Snap a measured packet rate (pps) to the nearest standard REAC sample rate.
 * pps = sample_rate / 12, so 3675 -> 44100, 4000 -> 48000, 8000 -> 96000.
 * Thresholds are the midpoints (3837.5, 6000). */
int reac_rate_snap(double pps);

/* Does a raw L2 frame look like REAC? Checks length and the EtherType at
 * bytes 12..13. Returns 1 if REAC, 0 otherwise. */
int reac_frame_is_reac(const uint8_t *frame, size_t len);

/* Strip the +2 FCS residue from a frame length, if present. One rule
 * covers both directions: a clean REAC frame is 52 + n*36 bytes (n = channel
 * width, 40 downstream / the box width upstream), so a length that is 52 + n*36
 * + 2 carries the trailer and comes back reduced by 2 (1494 -> 1492,
 * 1206 -> 1204, ...). Any other length (including every clean length) is
 * returned unchanged — the caller still validates the result as a frame; this
 * only normalizes a capture that kept two bytes of the FCS. */
size_t reac_frame_clean_len(size_t len);

/* The 16-bit little-endian sequence counter at bytes 14..15. The counter
 * increments once per frame and advances even across a lost frame, which makes
 * it a drop-immune rate/loss reference. Caller must ensure len >= 16. */
uint16_t reac_frame_counter(const uint8_t *frame);

/* Frames skipped between two consecutive counters (16-bit wrap-aware):
 * (cur - last - 1) & 0xFFFF. 0 = no loss. */
uint16_t reac_counter_gap(uint16_t last, uint16_t cur);

/* Measure the live packet rate on a bound AF_PACKET capture fd and snap it to a
 * standard REAC sample rate. Polls the fd for up to window_ms, counting REAC
 * frames, and returns the snapped rate (44100 / 48000 / 96000), or 0 if no REAC
 * traffic was seen in the window. NOTE: this consumes the frames it reads during
 * the window (call it on a fresh capture before starting a pipeline). The fd
 * should be an AF_PACKET socket bound to the REAC EtherType; it is set
 * non-blocking by this call. Linux only; returns -1 on a non-Linux build. */
int reac_detect_rate_fd(int fd, int window_ms);

/* ---- THE VERSION, DEFINED ONCE ------------------------------------------
 *
 * Here, in the public header, and nowhere else. It used to be in three places
 * with two different values - src/reac.c said 0.5.0 behind an overridable
 * #ifndef, openwrt/libreac/Makefile said 0.5.0, packaging/libreac.spec said
 * 0.6.0 - and none of them was reachable from a consumer at compile time, so
 * no version floor anywhere in the estate could fail. The RPM spec and the
 * OpenWrt recipe read the three numbers below; packaging/make-tarball.sh
 * refuses to build a tarball whose spec disagrees with them.
 *
 * The digits appear once. The string is built from them, so the two spellings
 * cannot drift.
 *
 * THE RULE (docs/layering.md): patch bumps until the control plane lands, and
 * the minor is what the control-plane extraction takes. reac_ctrlblk.h is that
 * extraction - the library holds conversation state now, not only layout.
 *
 * 0.7.0 IS AN API BREAK. reac_ctrl_build_name_frame() and
 * reac_ctrl_build_extra_frame() are gone; the identity record is one message
 * built by the identity-first surface that replaced them. The break first
 * shipped WITHOUT moving these digits or the soname, and every mechanism that
 * should have caught it was inert as a result: reac-pw's `>= 0.6.0` floor
 * accepted both libraries, rpm saw the same NEVRA and made `rpm -U` a no-op,
 * and the installed /usr/bin/reac-pw loaded the new libreac.so.0 and died on
 * `undefined symbol`. A removed symbol needs BOTH numbers below to move. */
#define LIBREAC_VERSION_MAJOR 0
#define LIBREAC_VERSION_MINOR 7
#define LIBREAC_VERSION_PATCH 2

/* THE SONAME'S MAJOR, and the second thing 0.7.0 had to move. The version
 * digits alone only stop a BUILD against the wrong headers; the soname is what
 * stops a RUN against the wrong shared object. While both libraries called
 * themselves libreac.so.0 the dynamic linker was happy to hand an old binary
 * the new library, and the error surfaced as a missing symbol at exec time
 * instead of a refused install. Bumping this makes the two co-installable and
 * the mismatch impossible: a binary linked against .so.1 will not load .so.0.
 *
 * The RPM spec (%%global abi) and the OpenWrt recipe (ABI_VERSION) read this
 * number; packaging/make-tarball.sh refuses a tarball whose spec disagrees. */
#define LIBREAC_ABI 1

#define LIBREAC__STR(x)  #x
#define LIBREAC__XSTR(x) LIBREAC__STR(x)
#define LIBREAC_VERSION                       \
	LIBREAC__XSTR(LIBREAC_VERSION_MAJOR) "."  \
	LIBREAC__XSTR(LIBREAC_VERSION_MINOR) "."  \
	LIBREAC__XSTR(LIBREAC_VERSION_PATCH)

/* Comparable, so a floor is one #if and not a strcmp nobody writes. */
#define LIBREAC_VERSION_NUM (LIBREAC_VERSION_MAJOR * 10000 + \
                             LIBREAC_VERSION_MINOR * 100 + \
                             LIBREAC_VERSION_PATCH)
#define LIBREAC_VERSION_AT_LEAST(ma, mi, pa) \
	(LIBREAC_VERSION_NUM >= ((ma) * 10000 + (mi) * 100 + (pa)))

/* The version of the library actually linked, which is the one a header floor
 * cannot check: a consumer built against this header can run against another
 * build. Compare it with LIBREAC_VERSION when that matters. */
const char *reac_version(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBREAC_REAC_H */
