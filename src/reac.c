// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// libreac — shared REAC protocol facts + helpers. The 96 kHz model is settled:
// {96000, 40, 12} at 8000 pps (double-pps, NOT channel-halving) — see
// https://github.com/FreeREAC/reac-protocol/blob/main/wire-format.md.

#define _POSIX_C_SOURCE 200809L
#include "reac/reac.h"
#include <string.h>

#ifndef LIBREAC_VERSION
#define LIBREAC_VERSION "0.1.0"
#endif

const struct reac_mode REAC_MODE_44K1 = { 44100, 40, 12 };
const struct reac_mode REAC_MODE_48K  = { 48000, 40, 12 };
const struct reac_mode REAC_MODE_96K  = { 96000, 40, 12 };

const struct reac_mode *reac_mode_for(int sample_rate)
{
	if (sample_rate == 96000) return &REAC_MODE_96K;
	if (sample_rate == 44100) return &REAC_MODE_44K1;
	return &REAC_MODE_48K; /* 48000 and anything unknown */
}

int reac_rate_snap(double pps)
{
	/* pps = rate/12: 44.1k=3675, 48k=4000, 96k=8000. Snap at the midpoints. */
	if (pps < 3837.5) return 44100; /* mid(3675, 4000) */
	if (pps < 6000.0) return 48000; /* mid(4000, 8000) */
	return 96000;
}

int reac_frame_is_reac(const uint8_t *frame, size_t len)
{
	if (!frame || len < 14)
		return 0;
	return frame[12] == 0x88 && frame[13] == 0x19; /* EtherType 0x8819 */
}

uint16_t reac_frame_counter(const uint8_t *frame)
{
	return (uint16_t)(frame[REAC_HDR_COUNTER_OFF] |
	                  ((uint16_t)frame[REAC_HDR_COUNTER_OFF + 1] << 8));
}

uint16_t reac_counter_gap(uint16_t last, uint16_t cur)
{
	return (uint16_t)((cur - last - 1) & 0xFFFF);
}

const char *reac_version(void)
{
	return LIBREAC_VERSION;
}

/* ---- live rate detection (Linux AF_PACKET) ---- */
#if defined(__linux__)
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <time.h>
#include <errno.h>

static double mono_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int reac_detect_rate_fd(int fd, int window_ms)
{
	if (fd < 0)
		return 0;
	/* non-blocking so poll() bounds the wait */
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0)
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);

	const double deadline = mono_s() + (window_ms > 0 ? window_ms : 1500) / 1000.0;
	uint8_t buf[2048];
	unsigned long frames = 0;
	double first = 0, last = 0;
	int have_first = 0;

	while (mono_s() < deadline) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
		int pr = poll(&pfd, 1, 100);
		if (pr <= 0)
			continue;
		for (;;) {
			ssize_t n = recv(fd, buf, sizeof buf, 0);
			if (n < 0)
				break; /* EAGAIN/EINTR: nothing more ready right now */
			if (!reac_frame_is_reac(buf, (size_t)n))
				continue;
			double now = mono_s();
			if (!have_first) { first = now; have_first = 1; }
			last = now;
			frames++;
		}
	}

	if (!have_first || frames < 50)
		return 0; /* no / too little REAC traffic in the window */
	double span = last - first;
	if (span <= 0)
		return 0;
	double pps = (double)(frames - 1) / span; /* frames-1 intervals over span */
	return reac_rate_snap(pps);
}

#else /* non-Linux */
int reac_detect_rate_fd(int fd, int window_ms)
{
	(void)fd; (void)window_ms;
	return -1;
}
#endif
