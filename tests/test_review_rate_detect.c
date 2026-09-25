// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* REVIEW 2026-09-25, finding M1 (docs/audits/2026-09-25-libreac-review.md).
 *
 * reac_detect_rate_fd() counts EVERY 0x8819 frame it reads and divides by the
 * span, so a socket that hears both directions of one 48 kHz session — the
 * master's 1492 B downstream AND a box's 628 B return, 4000 frames/s each —
 * measures 8000 frames/s and snaps to 96000. That is the ordinary case for the
 * socket reac_rx_open() hands it: reac_capture_open() does not ignore
 * PACKET_OUTGOING, so a host that is itself transmitting hears its own frames,
 * and an ungranted box's presence-flood is a broadcast every port hears.
 *
 * No NIC needed: an AF_UNIX datagram pair carries frames exactly as the
 * AF_PACKET socket would (one recv() = one frame), and a feeder thread paces
 * them on CLOCK_MONOTONIC absolute deadlines so the long-run rate is exact.
 *
 * The CONTROL arm (downstream only) must read 48000 first, or the harness is
 * broken and the verdict arm proves nothing (exit 2, NOT A RESULT). */
#define _GNU_SOURCE
#include <reac/reac.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PERIOD_NS   250000L          /* 4000 frames/s = 48 kHz */
#define FEED_MS     1000
#define WINDOW_MS   600

struct feeder {
	int fd;
	int with_upstream;
};

static void put_frame(uint8_t *f, size_t len, uint16_t counter)
{
	memset(f, 0, len);
	memset(f, 0xff, 6);
	f[12] = 0x88; f[13] = 0x19;
	f[14] = (uint8_t)counter; f[15] = (uint8_t)(counter >> 8);
	f[len - 2] = REAC_END_MARKER_0;
	f[len - 1] = REAC_END_MARKER_1;
}

static void *feed(void *arg)
{
	const struct feeder *fd = arg;
	static uint8_t down[REAC_FRAME_BYTES];
	static uint8_t up[REAC_UPSTREAM_OVERHEAD + 16 * REAC_UPSTREAM_BYTES_PER_CH];
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	const long ticks = (long)FEED_MS * 1000000L / PERIOD_NS;
	for (long i = 0; i < ticks; i++) {
		put_frame(down, sizeof down, (uint16_t)i);
		send(fd->fd, down, sizeof down, 0);
		if (fd->with_upstream) {
			put_frame(up, sizeof up, (uint16_t)i);
			send(fd->fd, up, sizeof up, 0);
		}
		t.tv_nsec += PERIOD_NS;
		while (t.tv_nsec >= 1000000000L) { t.tv_nsec -= 1000000000L; t.tv_sec++; }
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
	}
	return NULL;
}

static int run(int with_upstream)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0)
		return -2;
	int big = 4 << 20;
	setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
	setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
	struct feeder f = { .fd = sv[1], .with_upstream = with_upstream };
	pthread_t th;
	if (pthread_create(&th, NULL, feed, &f) != 0)
		return -2;
	int r = reac_detect_rate_fd(sv[0], WINDOW_MS);
	pthread_join(th, NULL);
	close(sv[0]);
	close(sv[1]);
	return r;
}

int main(void)
{
	int control = run(0);
	if (control != 48000) {
		printf("NOT A RESULT: test_review_rate_detect — the downstream-only control "
		       "read %d, not 48000; the harness is not pacing, so the verdict arm "
		       "would prove nothing\n", control);
		return 2;
	}
	int both = run(1);
	if (both != 48000) {
		fprintf(stderr, "FAIL test_review_rate_detect: a 48 kHz session heard in both "
		        "directions (1492 B downstream + 628 B return, 4000/s each) detects "
		        "as %d — every 0x8819 frame is counted, whatever its geometry\n", both);
		return 1;
	}
	printf("OK: test_review_rate_detect — both directions of a 48 kHz session detect as 48000\n");
	return 0;
}
