// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Did the capture get every frame? The control for an ABSENCE claim.
 *
 * "The box never re-broadcast the parameter" and "the capture dropped the frame
 * that did" are the same silence to any decoder. Every REAC frame carries a
 * 16-bit LE counter at [14:16] that steps by one per frame per talker, so a
 * dropped frame leaves a hole that can be counted. This prints, per source MAC,
 * how many steps were not +1 and how many frames those holes account for.
 *
 * A run with zero gaps is what lets a later report say the box sent nothing
 * rather than that nothing was recorded.
 */

#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/pcap_source.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define NT 8
struct talker { uint8_t mac[6]; int seen; uint16_t last; unsigned long frames, gaps, missing; };
static struct talker t[NT];
static int nt;

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: seq_gaps FILE.pcap\n"); return 2; }
	struct pcap_source ps;
	if (pcap_source_open(&ps, argv[1]) != 0) { fprintf(stderr, "cannot open\n"); return 1; }
	static uint8_t buf[4096];
	for (;;) {
		long n = pcap_source_next(&ps, buf, sizeof buf, NULL);
		if (n <= 0) break;
		if (!reac_frame_is_reac(buf, (size_t)n) || (size_t)n < 16) continue;
		struct talker *k = NULL;
		for (int i = 0; i < nt; i++) if (!memcmp(t[i].mac, buf + 6, 6)) k = &t[i];
		if (!k) { if (nt == NT) continue; k = &t[nt++]; memcpy(k->mac, buf + 6, 6); }
		uint16_t c = (uint16_t)(buf[14] | (buf[15] << 8));
		k->frames++;
		if (k->seen) {
			uint16_t step = (uint16_t)(c - k->last);
			if (step != 1) { k->gaps++; k->missing += (unsigned long)(uint16_t)(step - 1); }
		}
		k->seen = 1; k->last = c;
	}
	pcap_source_close(&ps);
	int any = 0;
	for (int i = 0; i < nt; i++) {
		printf("src=%02x:%02x:%02x:%02x:%02x:%02x frames=%lu gaps=%lu missing_frames=%lu\n",
		       t[i].mac[0], t[i].mac[1], t[i].mac[2], t[i].mac[3], t[i].mac[4], t[i].mac[5],
		       t[i].frames, t[i].gaps, t[i].missing);
		any = 1;
	}
	if (!any) { fprintf(stderr, "seq_gaps: no talker — an empty scan is not a result\n"); return 1; }
	return 0;
}
