// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test: reac_rx IS THE DOOR — it strips the capture path's +2, and nothing
 * behind it ever sees a residue byte.
 *
 * Since 2026-09-21 the parsers REFUSE a residue length instead of stripping it
 * themselves (the residue is the tap's and does not occur on a REAC network: 0
 * in 592,762 frames off a plain NIC, census in <reac/reac.h>). That moved the
 * strip into reac_rx's loop, where the whole stream gate / duplicate guard /
 * decode chain now runs on the clean length. If that one line ever goes away,
 * every mirror twin stops being a DUPLICATE and becomes NOT OURS — the frame
 * count halves and the rate estimate follows it, silently. Nothing in this repo
 * could see that before this file.
 *
 * The fixture is a pcap of N distinct frames, each followed by its FAITHFUL
 * mirror twin: the same bytes plus the low 16 bits of the frame's own Ethernet
 * FCS, little-endian, which is exactly what a both-directions port mirror
 * delivers. Both directions are exercised, because the gate treats them
 * differently (1492 exactly vs the box-width geometry).
 *
 * AND IT CARRIES ITS CONTROL. The same frames WITHOUT twins must read dups = 0:
 * a `dups` counter that also fires on distinct frames would pass the arm above
 * while proving nothing. Sabotage-verified 2026-09-21 by making the loop skip
 * the strip — upstream went ok=494 dup=0 other=493, downstream ok=264 dup=0
 * other=263, i.e. every twin rejected at the gate, and both arms went red.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

#include <reac/reac.h>
#include <reac/reac_braid.h>
#include <reac/transport/reac_rx.h>
#include <reac/transport/reac_ring.h>

#include "upstream_fixtures.inc"

#define NFRAMES  40
#define CTR_BASE 0xffec   /* the run straddles the u16 counter wrap */

static int fails;
#define CHK(c) do { \
	if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
} while (0)

static void pcap_hdr(FILE *f)
{
	uint32_t gh[6] = { 0xa1b2c3d4, 0x00040002, 0, 0, 65535, 1 };
	fwrite(gh, sizeof gh, 1, f);
}

static void pcap_rec(FILE *f, const uint8_t *frame, uint32_t len)
{
	uint32_t rh[4] = { 0, 0, len, len };
	fwrite(rh, sizeof rh, 1, f);
	fwrite(frame, len, 1, f);
}

/* Ethernet FCS (CRC-32/ISO-HDLC). The residue is the frame's OWN FCS, so the
 * fixture computes it rather than appending two arbitrary bytes. */
static uint32_t fcs32(const uint8_t *p, size_t n)
{
	uint32_t crc = 0xffffffffu;
	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1));
	}
	return ~crc;
}

static void add_fcs_residue(uint8_t *out, size_t len)
{
	uint32_t fcs = fcs32(out, len);
	out[len]     = (uint8_t)(fcs & 0xff);
	out[len + 1] = (uint8_t)((fcs >> 8) & 0xff);
}

/* The i-th distinct upstream frame: the captured S-1608 return, counter
 * CTR_BASE+i, one audio byte marked so no two frames are byte-equal. */
static void mk_up(uint8_t *out, int i)
{
	memcpy(out, UP16, sizeof UP16);
	uint16_t c = (uint16_t)(CTR_BASE + i);
	out[14] = (uint8_t)(c & 0xff);
	out[15] = (uint8_t)(c >> 8);
	out[REAC_L2_HEADER_LEN] = (uint8_t)i;
}

static void mk_down(uint8_t *out, uint16_t counter)
{
	memset(out, 0, REAC_FRAME_BYTES);
	memset(out, 0xff, 6);                       /* broadcast */
	static const uint8_t master[6] = { 0x00, 0x40, 0xab, 0xc4, 0x91, 0x90 };
	memcpy(out + 6, master, 6);
	out[12] = 0x88; out[13] = 0x19;
	out[14] = (uint8_t)(counter & 0xff); out[15] = (uint8_t)(counter >> 8);
	for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
		size_t pos[3];
		reac_braid_pos(s, 0, REAC_MAX_CHANNELS, pos);
		out[REAC_L2_HEADER_LEN + pos[0]] = (uint8_t)counter;
	}
	out[REAC_FRAME_BYTES - 2] = REAC_END_MARKER_0;
	out[REAC_FRAME_BYTES - 1] = REAC_END_MARKER_1;
}

static int run_rx(struct reac_rx *rx, uint64_t want_ok)
{
	if (reac_rx_start(rx) != 0)
		return -1;
	for (int i = 0; i < 2000; i++) {
		if (atomic_load(&rx->frames_ok) >= want_ok)
			break;
		struct timespec ts = { 0, 1000000 };
		nanosleep(&ts, NULL);
	}
	reac_rx_stop(rx);
	return atomic_load(&rx->frames_ok) >= want_ok ? 0 : -1;
}

/* Replay N frames (with or without their twins) and report the counters. */
static void replay(const char *what, int downstream, int twins)
{
	char path[] = "/tmp/libreac-rx-twin-XXXXXX";
	int fd = mkstemp(path);
	CHK(fd >= 0);
	if (fd < 0)
		return;
	FILE *f = fdopen(fd, "wb");
	CHK(f != NULL);
	if (!f) {
		close(fd);
		return;
	}
	pcap_hdr(f);
	uint8_t up[sizeof UP16 + 2];
	uint8_t down[REAC_FRAME_BYTES + 2];
	for (int i = 0; i < NFRAMES; i++) {
		if (downstream) {
			mk_down(down, (uint16_t)(CTR_BASE + i));
			pcap_rec(f, down, REAC_FRAME_BYTES);
			if (twins) {
				add_fcs_residue(down, REAC_FRAME_BYTES);
				pcap_rec(f, down, REAC_FRAME_BYTES + 2);
			}
		} else {
			mk_up(up, i);
			pcap_rec(f, up, (uint32_t)sizeof UP16);
			if (twins) {
				add_fcs_residue(up, sizeof UP16);
				pcap_rec(f, up, (uint32_t)(sizeof UP16 + 2));
			}
		}
	}
	fclose(f);

	struct reac_rx_cfg cfg = { .kind = REAC_RX_PCAP, .source = path,
	                           .forced_rate = 48000, .pcap_realtime = 0,
	                           .accept = downstream ? REAC_RX_ACCEPT_DOWNSTREAM
	                                                : REAC_RX_ACCEPT_UPSTREAM };
	struct reac_ring ring;
	struct reac_rx rx;
	CHK(reac_rx_open(&rx, &cfg, &ring) == 0);
	CHK(run_rx(&rx, NFRAMES) == 0);
	uint64_t ok  = atomic_load(&rx.frames_ok);
	uint64_t dup = atomic_load(&rx.frames_dup);
	uint64_t oth = atomic_load(&rx.frames_other);
	uint64_t bad = atomic_load(&rx.frames_bad);
	printf("  %-10s %-7s ok=%llu dup=%llu other=%llu bad=%llu\n",
	       what, twins ? "+twins" : "clean",
	       (unsigned long long)ok, (unsigned long long)dup,
	       (unsigned long long)oth, (unsigned long long)bad);

	CHK(ok >= NFRAMES);
	/* The residue copy is DROPPED AS A DUPLICATE, never refused as "not ours":
	 * the gate sees the clean length, so both copies of a twin are the same
	 * shape and the guard collapses the pair. The feeder may stop between a
	 * frame and its twin, hence the one-frame tolerance. */
	CHK(oth == 0);
	CHK(bad == 0);
	if (twins)
		CHK(dup + 1 >= ok);
	else
		CHK(dup == 0);   /* THE CONTROL: distinct frames are not duplicates */
	reac_rx_close(&rx);
	unlink(path);
}

/* A 40-wide BOX's return: the same 1492 B as the desk's downstream, UNICAST to
 * that desk (operator ruling 2026-09-25: a box may fill the fabric). */
static void mk_box40(uint8_t *out, uint16_t counter)
{
	mk_down(out, counter);
	static const uint8_t master[6] = { 0x00, 0x40, 0xab, 0xc4, 0x91, 0x90 };
	static const uint8_t box[6]    = { 0x00, 0x40, 0xab, 0x40, 0x40, 0x01 };
	memcpy(out, master, 6);                     /* unicast, to the desk */
	memcpy(out + 6, box, 6);
}

/* DIRECTION, NOT WIDTH: the desk's downstream and a 40-wide box's return are the
 * same length on one wire, interleaved. Each gate takes its own stream only —
 * the downstream the BROADCAST one, the upstream the UNICAST one — and refuses
 * the other as "not ours". Before the ruling's fix the upstream gate refused
 * every 1492 B frame and the downstream gate took both. */
static void replay_same_width(int downstream)
{
	char path[] = "/tmp/libreac-rx-dir-XXXXXX";
	int fd = mkstemp(path);
	CHK(fd >= 0);
	if (fd < 0)
		return;
	FILE *f = fdopen(fd, "wb");
	if (!f) {
		close(fd);
		return;
	}
	pcap_hdr(f);
	uint8_t fr[REAC_FRAME_BYTES];
	for (int i = 0; i < NFRAMES; i++) {
		mk_down(fr, (uint16_t)(CTR_BASE + i));
		pcap_rec(f, fr, REAC_FRAME_BYTES);
		mk_box40(fr, (uint16_t)(0x1000 + i));
		pcap_rec(f, fr, REAC_FRAME_BYTES);
	}
	fclose(f);

	struct reac_rx_cfg cfg = { .kind = REAC_RX_PCAP, .source = path,
	                           .forced_rate = 48000, .pcap_realtime = 0,
	                           .accept = downstream ? REAC_RX_ACCEPT_DOWNSTREAM
	                                                : REAC_RX_ACCEPT_UPSTREAM };
	struct reac_ring ring;
	struct reac_rx rx;
	CHK(reac_rx_open(&rx, &cfg, &ring) == 0);
	CHK(run_rx(&rx, NFRAMES) == 0);
	/* let the feeder finish the file: the refusals come interleaved */
	uint64_t ok  = atomic_load(&rx.frames_ok);
	uint64_t oth = atomic_load(&rx.frames_other);
	printf("  %-10s 40+40   ok=%llu other=%llu\n", downstream ? "downstream" : "upstream",
	       (unsigned long long)ok, (unsigned long long)oth);
	/* The replay loops the file, so the counts are per pass: one stream in, the
	 * other refused, the same number of each. */
	CHK(ok >= NFRAMES && ok <= oth + 1 && oth <= ok + 1);
	/* ...and the one taken is the right one: its own counters, its own source. */
	const uint16_t last = rx.ppm_last_counter;
	const int box_counter = last >= 0x1000 && last < 0x1000 + NFRAMES;
	static const uint8_t box[6] = { 0x00, 0x40, 0xab, 0x40, 0x40, 0x01 };
	if (downstream)
		CHK(!box_counter);
	else
		CHK(box_counter && rx.up_src_locked && memcmp(rx.up_src, box, 6) == 0);
	reac_rx_close(&rx);
	unlink(path);
}

int main(void)
{
	printf("test_rx_twin: %d distinct frames, with and without their FCS-residue "
	       "mirror twins, both directions\n", NFRAMES);
	replay("upstream", 0, 0);
	replay("upstream", 0, 1);
	replay("downstream", 1, 0);
	replay("downstream", 1, 1);
	replay_same_width(1);
	replay_same_width(0);

	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("OK: reac_rx strips the capture's +2 at the door — every mirror twin is a "
	       "DUPLICATE (not 'not ours'), every frame decodes, and a stream without twins "
	       "reports no duplicates at all\n");
	return 0;
}
