// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Who talks on this segment, and in what shapes. The census that has to run
 * BEFORE any claim that something is absent: an upstream answer that never
 * appears and an upstream talker that was never captured look identical in a
 * per-record search. This prints, per source MAC, the frame types and the
 * ORIGINAL frame lengths seen, so "the box sent only heartbeats" is separated
 * from "the box was not on this capture".
 */

#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/pcap_source.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define NT 32
struct talker {
	uint8_t mac[6];
	unsigned long frames;
	unsigned long marker[4];     /* 0 cdea, 1 cfea, 2 0000 filler, 3 other */
	unsigned long kind[REAC_CTRL_UNKNOWN_CTRL + 1];
	unsigned lens[16]; unsigned long lenc[16]; int nlen;
	uint64_t first_us, last_us;
};
static struct talker t[NT];
static int nt;

static struct talker *find(const uint8_t *mac)
{
	for (int i = 0; i < nt; i++)
		if (!memcmp(t[i].mac, mac, 6)) return &t[i];
	if (nt == NT) return NULL;
	memcpy(t[nt].mac, mac, 6);
	return &t[nt++];
}

static void addlen(struct talker *k, unsigned l)
{
	for (int i = 0; i < k->nlen; i++)
		if (k->lens[i] == l) { k->lenc[i]++; return; }
	if (k->nlen == 16) return;
	k->lens[k->nlen] = l; k->lenc[k->nlen++] = 1;
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: wire_census FILE.pcap\n"); return 2; }
	struct pcap_source ps;
	if (pcap_source_open(&ps, argv[1]) != 0) { fprintf(stderr, "cannot open\n"); return 1; }
	static uint8_t buf[4096];
	uint64_t t0 = 0;
	unsigned long records = 0, reac = 0;
	for (;;) {
		uint64_t ts = 0;
		long n = pcap_source_next(&ps, buf, sizeof buf, &ts);
		if (n <= 0) break;
		records++;
		if (!t0) t0 = ts;
		if (!reac_frame_is_reac(buf, (size_t)n)) continue;
		reac++;
		struct talker *k = find(buf + 6);
		if (!k) continue;
		if (!k->frames) k->first_us = ts;
		k->last_us = ts;
		k->frames++;
		addlen(k, ps.last_orig_len);
		if ((size_t)n >= 18) {
			if (buf[16] == 0xcd && buf[17] == 0xea) k->marker[0]++;
			else if (buf[16] == 0xcf && buf[17] == 0xea) k->marker[1]++;
			else if (buf[16] == 0x00 && buf[17] == 0x00) k->marker[2]++;
			else k->marker[3]++;
		}
		struct reac_ctrl_parsed p;
		enum reac_ctrl_kind kk = reac_ctrl_parse(buf, (size_t)n, &p);
		if ((int)kk <= REAC_CTRL_UNKNOWN_CTRL) k->kind[kk]++;
	}
	pcap_source_close(&ps);
	printf("records=%lu reac=%lu talkers=%d span=%.3fs\n", records, reac, nt,
	       nt ? 0.0 : 0.0);
	for (int i = 0; i < nt; i++) {
		printf("src=%02x:%02x:%02x:%02x:%02x:%02x frames=%lu t=[%.3f..%.3f] "
		       "cdea=%lu cfea=%lu filler=%lu other=%lu\n  origlens:",
		       t[i].mac[0], t[i].mac[1], t[i].mac[2], t[i].mac[3], t[i].mac[4], t[i].mac[5],
		       t[i].frames, (double)(t[i].first_us - t0)/1e6, (double)(t[i].last_us - t0)/1e6,
		       t[i].marker[0], t[i].marker[1], t[i].marker[2], t[i].marker[3]);
		for (int l = 0; l < t[i].nlen; l++) printf(" %u(%lu)", t[i].lens[l], t[i].lenc[l]);
		printf("\n  kinds:");
		for (int b = 0; b <= REAC_CTRL_UNKNOWN_CTRL; b++)
			if (t[i].kind[b]) printf(" %s=%lu", reac_ctrl_kind_name((enum reac_ctrl_kind)b), t[i].kind[b]);
		printf("\n");
	}
	if (!reac) { fprintf(stderr, "wire_census: no REAC frame — an empty scan is not a result\n"); return 1; }
	return 0;
}
