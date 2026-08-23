// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Every byte the BOX's own frames change, and when — the upstream half of a
 * head-amp granularity question.
 *
 * The box answers a head-amp write somewhere or it does not answer at all, and
 * a search that only looks at head-amp-shaped records assumes the format of an
 * answer nobody has seen. So this looks at the box's frames byte by byte over
 * the whole capture and reports the RARE movers: a position that changes on
 * nearly every frame is audio, a position that changes once or twice is state.
 *
 * ITS OWN POSITIVE CONTROL IS BUILT IN. The audio region moves constantly, and
 * the summary prints how many positions are in that class. If it were zero the
 * instrument would be reading nothing, and a report of "no state changed" over
 * a stream it cannot see is the failure this guards against. Truncation is
 * printed for the same reason: snaplen 200 means only [0:200) of a 628-byte
 * upstream frame is here, so this can only speak for the window it saw and
 * says which window that is.
 */

#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/pcap_source.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN 256
#define MAXCHG 40

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: upstream_watch FILE.pcap AA:BB:CC:DD:EE:FF\n");
		return 2;
	}
	unsigned m[6];
	if (sscanf(argv[2], "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
		fprintf(stderr, "upstream_watch: bad MAC\n");
		return 2;
	}
	uint8_t mac[6];
	for (int i = 0; i < 6; i++) mac[i] = (uint8_t)m[i];

	struct pcap_source ps;
	if (pcap_source_open(&ps, argv[1]) != 0) { fprintf(stderr, "cannot open\n"); return 1; }

	static uint8_t buf[4096];
	static uint8_t prev[WIN];
	static unsigned long nchg[WIN];
	static double chg_t[WIN][MAXCHG];
	static uint8_t chg_a[WIN][MAXCHG], chg_b[WIN][MAXCHG];
	int have = 0;
	size_t win = 0;
	uint64_t t0 = 0;
	unsigned long frames = 0, mine = 0, minlen = ~0UL, maxlen = 0, minorig = ~0UL, maxorig = 0;

	for (;;) {
		uint64_t ts = 0;
		long n = pcap_source_next(&ps, buf, sizeof buf, &ts);
		if (n <= 0) break;
		frames++;
		if (!t0) t0 = ts;
		if (memcmp(buf + 6, mac, 6)) continue;
		if (!reac_frame_is_reac(buf, (size_t)n)) continue;
		mine++;
		if ((unsigned long)n < minlen) minlen = (unsigned long)n;
		if ((unsigned long)n > maxlen) maxlen = (unsigned long)n;
		if (ps.last_orig_len < minorig) minorig = ps.last_orig_len;
		if (ps.last_orig_len > maxorig) maxorig = ps.last_orig_len;
		size_t w = (size_t)n < WIN ? (size_t)n : WIN;
		if (!have) { have = 1; win = w; memcpy(prev, buf, w); continue; }
		if (w < win) win = w;
		for (size_t i = 0; i < win; i++) {
			if (prev[i] == buf[i]) continue;
			if (nchg[i] < MAXCHG) {
				chg_t[i][nchg[i]] = (double)(ts - t0) / 1e6;
				chg_a[i][nchg[i]] = prev[i];
				chg_b[i][nchg[i]] = buf[i];
			}
			nchg[i]++;
			prev[i] = buf[i];
		}
	}
	pcap_source_close(&ps);

	printf("frames_total=%lu from_mac=%lu caplen=[%lu..%lu] origlen=[%lu..%lu] window=%zu\n",
	       frames, mine, minlen, maxlen, minorig, maxorig, win);
	unsigned busy = 0, quiet = 0, still = 0;
	for (size_t i = 0; i < win; i++) {
		if (nchg[i] == 0) { still++; continue; }
		if (nchg[i] > (mine / 8)) { busy++; continue; }
		quiet++;
	}
	printf("byte classes over [0,%zu): never_changed=%u rare_movers=%u "
	       "busy(audio-like)=%u\n", win, still, quiet, busy);
	printf("-- positive control: busy positions must be non-zero or this tool read nothing --\n");
	for (size_t i = 0; i < win; i++) {
		if (nchg[i] == 0 || nchg[i] > (mine / 8)) continue;
		printf("byte[%03zu] changes=%lu :", i, nchg[i]);
		unsigned long k = nchg[i] < MAXCHG ? nchg[i] : MAXCHG;
		for (unsigned long j = 0; j < k; j++)
			printf(" %.6f(%02x->%02x)", chg_t[i][j], chg_a[i][j], chg_b[i][j]);
		if (nchg[i] > MAXCHG) printf(" ...");
		printf("\n");
	}
	if (!mine) {
		fprintf(stderr, "upstream_watch: that MAC sent nothing — an empty scan is not a result\n");
		return 1;
	}
	return 0;
}
