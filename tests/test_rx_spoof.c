// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* A FRAME SENT AS THE BOX IS NOT DECODED AS THE BOX'S UNLESS IT CONTINUES THE BOX'S COUNT.
 *
 * reac_rx's gate locks onto the box's source MAC, and a host on the same wire can send with
 * that MAC. Before the counter rule every such frame was decoded into the capture ring (a
 * counter that went backwards was only counted as a gap). Replayed through the real reac_rx
 * from a pcap of the captured S-1608 return:
 *
 *   A. forged frames carrying counters BEHIND the box's are dropped, never decoded: each
 *      pass decodes the box's N frames and counts N others;
 *   B. a frame carrying the SAME counter as the box's last one, with other bytes, is the
 *      conflict, dropped likewise;
 *   C. a box that RESTARTS its counter is followed after REAC_RX_RESYNC_RUN frames that
 *      continue each other: 7 dropped, the rest decoded, not the whole restarted run;
 *   D. THE CONTROL: the box's own stream alone drops nothing.
 *
 * Sabotage: drop the counter rule from reac_rx's loop and A and B read others = 0 (red);
 * make the resync run unreachable and C reads others >= M (red). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

#include <reac/reac.h>
#include <reac/transport/reac_rx.h>
#include <reac/transport/reac_ring.h>

#include "upstream_fixtures.inc"

#define N 40

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

/* The captured S-1608 return with counter `c` and one audio byte set to `mark`, so no two
 * frames written here are byte-equal (the duplicate guard stays out of it). */
static void up(FILE *f, uint16_t c, uint8_t mark)
{
	uint8_t out[sizeof UP16];
	memcpy(out, UP16, sizeof UP16);
	out[14] = (uint8_t)(c & 0xff);
	out[15] = (uint8_t)(c >> 8);
	out[REAC_L2_HEADER_LEN] = mark;
	pcap_rec(f, out, (uint32_t)sizeof out);
}

struct counts { uint64_t ok, other, dup, bad; };

/* Replay the pcap at `path` through reac_rx until `want_ok` frames decoded. The pcap
 * source loops at EOF and forgets the counter at the seam, so every pass is the same. */
static struct counts replay(const char *path, uint64_t want_ok)
{
	struct counts r = { 0 };
	struct reac_rx_cfg cfg = { .kind = REAC_RX_PCAP, .source = path,
	                           .forced_rate = 48000, .pcap_realtime = 0,
	                           .accept = REAC_RX_ACCEPT_UPSTREAM };
	struct reac_ring ring;
	struct reac_rx rx;
	CHK(reac_rx_open(&rx, &cfg, &ring) == 0);
	CHK(reac_rx_start(&rx) == 0);
	for (int i = 0; i < 3000 && atomic_load(&rx.frames_ok) < want_ok; i++) {
		struct timespec ts = { 0, 1000000 };
		nanosleep(&ts, NULL);
	}
	reac_rx_stop(&rx);
	r.ok = atomic_load(&rx.frames_ok);
	r.other = atomic_load(&rx.frames_other);
	r.dup = atomic_load(&rx.frames_dup);
	r.bad = atomic_load(&rx.frames_bad);
	reac_rx_close(&rx);
	return r;
}

static FILE *open_pcap(char *path)
{
	int fd = mkstemp(path);
	CHK(fd >= 0);
	if (fd < 0)
		return NULL;
	FILE *f = fdopen(fd, "wb");
	CHK(f != NULL);
	if (f)
		pcap_hdr(f);
	return f;
}

static void show(const char *what, struct counts r)
{
	printf("  %-34s ok=%llu other=%llu dup=%llu bad=%llu\n", what,
	       (unsigned long long)r.ok, (unsigned long long)r.other,
	       (unsigned long long)r.dup, (unsigned long long)r.bad);
}

int main(void)
{
	const uint16_t base = 0xfff0;   /* the run straddles the u16 counter wrap */

	/* A. a forged frame three counts behind after every box frame */
	{
		char path[] = "/tmp/libreac-rx-spoof-XXXXXX";
		FILE *f = open_pcap(path);
		if (f) {
			for (int i = 0; i < N; i++) {
				up(f, (uint16_t)(base + i), (uint8_t)i);
				up(f, (uint16_t)(base + i - 3), 0xee);
			}
			fclose(f);
			struct counts r = replay(path, N);
			show("A. behind the box's counter", r);
			CHK(r.ok >= N);
			/* the frames alternate box, forged: per frame decoded, one dropped */
			CHK(r.other + 1 >= r.ok && r.other <= r.ok);
			CHK(r.bad == 0);
			unlink(path);
		}
	}

	/* B. a forged frame with the box's own counter, other bytes, after every box frame */
	{
		char path[] = "/tmp/libreac-rx-spoof-XXXXXX";
		FILE *f = open_pcap(path);
		if (f) {
			for (int i = 0; i < N; i++) {
				up(f, (uint16_t)(base + i), (uint8_t)i);
				up(f, (uint16_t)(base + i), 0xee);
			}
			fclose(f);
			struct counts r = replay(path, N);
			show("B. the box's counter, other bytes", r);
			CHK(r.ok >= N);
			CHK(r.other + 1 >= r.ok && r.other <= r.ok);
			unlink(path);
		}
	}

	/* C. the box restarts its counter: N frames, then M frames counting again from 0 */
	{
		const int M = 40;
		char path[] = "/tmp/libreac-rx-spoof-XXXXXX";
		FILE *f = open_pcap(path);
		if (f) {
			for (int i = 0; i < N; i++)
				up(f, (uint16_t)(1000 + i), (uint8_t)i);
			for (int j = 0; j < M; j++)
				up(f, (uint16_t)j, (uint8_t)(0x80 + j));
			fclose(f);
			/* Each pass drops 7 and decodes N + M - 7 when the restart is followed; were
			 * it not, each pass would drop all M and decode only N (others ~ decoded). The
			 * replay loops many passes, so the ratio is the verdict. */
			struct counts r = replay(path, (uint64_t)(N + M - 7));
			show("C. the box restarts its counter", r);
			CHK(r.ok >= (uint64_t)(N + M - 7));
			CHK(r.other >= 7);
			CHK(r.other * 5 < r.ok);
			unlink(path);
		}
	}

	/* D. THE CONTROL: the box alone */
	{
		char path[] = "/tmp/libreac-rx-spoof-XXXXXX";
		FILE *f = open_pcap(path);
		if (f) {
			for (int i = 0; i < N; i++)
				up(f, (uint16_t)(base + i), (uint8_t)i);
			fclose(f);
			struct counts r = replay(path, N);
			show("D. control: the box alone", r);
			CHK(r.ok >= N);
			CHK(r.other == 0 && r.dup == 0 && r.bad == 0);
			unlink(path);
		}
	}

	if (fails) {
		fprintf(stderr, "%d rx-spoof check(s) failed\n", fails);
		return 1;
	}
	printf("OK: test_rx_spoof — frames sent as the box with a counter behind or equal to the "
	       "box's are dropped and never decoded; a box restarting its counter is followed after "
	       "7 frames; the box's own stream drops nothing\n");
	return 0;
}
