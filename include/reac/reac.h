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

/* OHRCA-generation consoles (M-5000/M-480; also the S-4000S merge units) append
 * a 2-byte per-frame CRC-16 trailer AFTER the C2 EA end marker, in BOTH
 * directions (measured on live M-5000 downstream captures 2026-07-11 and on the
 * S-4000 32-ch upstream, matrix-m200-s4000 2026-07-24). The trailer is not part
 * of the frame the decoders read — strip it with reac_frame_clean_len(). */
#define REAC_FRAME_BYTES_OHRCA (REAC_FRAME_BYTES + 2)  /* 1494: OHRCA downstream */

/* UPSTREAM (stagebox -> master) frame geometry — box-width sized:
 *     frame_len = REAC_UPSTREAM_OVERHEAD + n_channels * REAC_UPSTREAM_BYTES_PER_CH
 *     S-1608 -> 16 ch -> 628 B;  S-0808 -> 8 ch -> 340 B;  S-4000 -> 32 ch -> 1204 B
 * (the downstream 1492 B broadcast is the 40-ch solution of the same formula). */
#define REAC_UPSTREAM_OVERHEAD     52  /* 50 B header + 2 B end marker */
#define REAC_UPSTREAM_BYTES_PER_CH 36  /* 12 samples x 3 B */

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

/* Strip the OHRCA +2 CRC trailer from a frame length, if present. One rule
 * covers both directions: a clean REAC frame is 52 + n*36 bytes (n = channel
 * width, 40 downstream / the box width upstream), so a length that is 52 + n*36
 * + 2 carries the trailer and comes back reduced by 2 (1494 -> 1492,
 * 1206 -> 1204, ...). Any other length (including every clean length) is
 * returned unchanged — the caller still validates the result as a frame; this
 * only normalizes the OHRCA variant. */
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

/* Library version string, e.g. "0.1.0". */
const char *reac_version(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBREAC_REAC_H */
