// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// libreac — shared REAC protocol facts + helpers. The 96 kHz model is settled:
// {96000, 40, 12} at 8000 pps (double-pps, NOT channel-halving) — see
// https://github.com/FreeREAC/reac-protocol/blob/main/wire-format.md.

#define _POSIX_C_SOURCE 200809L
#include "reac/reac.h"
#include "reac/reac_cfg.h"   /* the closed rate list */
#include <string.h>


/* The three rates are the closed list <reac/reac_cfg.h> declares; the geometry is
 * reac.h's. Nothing here spells either again. */
const struct reac_mode REAC_MODE_44K1 = { REAC_CFG_RATE_44100, REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT };
const struct reac_mode REAC_MODE_48K  = { REAC_CFG_RATE_48000, REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT };
const struct reac_mode REAC_MODE_96K  = { REAC_CFG_RATE_96000, REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT };

const struct reac_mode *reac_mode_for(int sample_rate)
{
	if (sample_rate == REAC_CFG_RATE_96000) return &REAC_MODE_96K;
	if (sample_rate == REAC_CFG_RATE_44100) return &REAC_MODE_44K1;
	return &REAC_MODE_48K; /* 48000 and anything unknown */
}

int reac_rate_snap(double pps)
{
	/* pps = rate/12: 44.1k=3675, 48k=4000, 96k=8000. Snap at the midpoints. */
	const double p44 = (double)REAC_CFG_RATE_44100 / REAC_SAMPLES_PER_PKT;
	const double p48 = (double)REAC_CFG_RATE_48000 / REAC_SAMPLES_PER_PKT;
	const double p96 = (double)REAC_CFG_RATE_96000 / REAC_SAMPLES_PER_PKT;
	if (pps < (p44 + p48) / 2) return REAC_CFG_RATE_44100; /* 3837.5 */
	if (pps < (p48 + p96) / 2) return REAC_CFG_RATE_48000; /* 6000 */
	return REAC_CFG_RATE_96000;
}

/* The pace code a master announces and a box follows (reac.h carries the law and
 * the measurements). Kept as three explicit bands rather than a table so an fps
 * that is not one of the three legal paces still lands on a defined class instead
 * of reading past a table's end; 44.1 kHz is the LOW band because its frame rate
 * is the low one (3675), not because it is a fallback. */
uint8_t reac_pace_code(int fps)
{
	if (fps >= 8000) return 1;   /* 96 kHz   */
	if (fps <= 3700) return 2;   /* 44.1 kHz (3675 frames/s) */
	return 0;                    /* 48 kHz   */
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

size_t reac_frame_clean_len(size_t len)
{
	/* clean frame = 52 + n*36; a capture may leave 2 bytes of FCS after the
	 * end marker, so a
	 * trailered length is congruent to 2 mod 36 past the overhead. */
	if (len >= REAC_UPSTREAM_OVERHEAD + 2 &&
	    (len - REAC_UPSTREAM_OVERHEAD) % REAC_UPSTREAM_BYTES_PER_CH == 2)
		return len - 2;
	return len;
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

/* ONE STREAM, MEASURED BY ITS OWN COUNTER. A socket can hear more than one
 * cadence of the same session: a master's downstream and a box's return, its own
 * transmissions (PACKET_OUTGOING), or each frame twice off a mirrored port. Counting
 * every 0x8819 frame therefore reads 2x (48 kHz detected as 96 kHz). So the rate is
 * taken from ONE source MAC's frames, preferring the 40-channel master downstream
 * when one is heard (that IS the pace), and from the advance of that stream's own
 * sequence counter rather than from how many copies arrived: a repeated counter adds
 * nothing and a lost frame still advances it. */
struct rate_track {
	int      have;
	uint8_t  mac[6];
	uint16_t last_counter;
	unsigned long advance;   /* counter steps since the first frame */
	double   first, last;
};

static void rate_track_feed(struct rate_track *t, const uint8_t *f, double now)
{
	uint16_t c = reac_frame_counter(f);
	if (!t->have) {
		memcpy(t->mac, f + 6, 6);
		t->last_counter = c;
		t->first = t->last = now;
		t->have = 1;
		return;
	}
	if (memcmp(t->mac, f + 6, 6) != 0)
		return;                      /* another stream: not this one's pace */
	uint16_t step = (uint16_t)(c - t->last_counter);
	if (step == 0 || step > 0x8000)
		return;                      /* a duplicate, or a stale/reordered copy */
	t->advance += step;
	t->last_counter = c;
	t->last = now;
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
	struct rate_track down = { 0 }, other = { 0 };

	while (mono_s() < deadline) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
		int pr = poll(&pfd, 1, 100);
		if (pr <= 0)
			continue;
		for (;;) {
			ssize_t n = recv(fd, buf, sizeof buf, 0);
			if (n < 0)
				break; /* EAGAIN/EINTR: nothing more ready right now */
			if (n < REAC_HDR_COUNTER_OFF + 2 || !reac_frame_is_reac(buf, (size_t)n))
				continue;
			double now = mono_s();
			if (reac_frame_is_master_downstream(reac_frame_clean_len((size_t)n)))
				rate_track_feed(&down, buf, now);
			else
				rate_track_feed(&other, buf, now);
		}
	}

	/* The master's downstream when there is enough of it; any single stream else. */
	const struct rate_track *t = down.advance >= 50 ? &down : &other;
	if (t->advance < 50)
		return 0; /* no / too little REAC traffic in the window */
	double span = t->last - t->first;
	if (span <= 0)
		return 0;
	double pps = (double)t->advance / span; /* counter steps over span */
	return reac_rate_snap(pps);
}

#else /* non-Linux */
int reac_detect_rate_fd(int fd, int window_ms)
{
	(void)fd; (void)window_ms;
	return -1;
}
#endif
