// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Every ENROLL GROUP MAP on a capture, byte for byte, with its talker — and the
 * controls that make an ABSENCE readable.
 *
 * THE QUESTION IT ANSWERS: libreac generates the ENROLL group map
 * (cdea, link 1, opcode 0x10) as a pure function of width — 1x0x41 for an 8-input
 * box, 2x for 16, 4x for 32. The 8- and 32-wide rows are corpus-measured; the
 * 16-wide row for an S-1608 is a PREDICTION of the rule and nothing had ever been
 * compared against a desk. This tool extracts the real ones.
 *
 * WHY IT IS NOT wire_census + grep: a group map is one 34-byte template inside a
 * 1492-byte frame, it appears a handful of times per session, and the finding here
 * is usually that a shape is MISSING. A missing shape and an unread file look the
 * same, so every run prints three controls in the same pass:
 *   - records / reac / vlan counts (a tag-aware read: since 2026-09 the corpus is
 *     802.1Q VLAN 12 and libreac's own pcap_source hands those frames back with the
 *     tag still on, so reac_frame_is_reac rejects every one of them);
 *   - a per-talker census (who was on the wire at all);
 *   - the OTHER control opcodes each talker sent (so "no group map from this desk"
 *     is separated from "this desk was silent").
 * A run that finds zero REAC frames says so and exits non-zero.
 *
 * Layout (reac_ctrlblk.h): the 32-byte control block sits at frame[18:50]; the two
 * bytes before it are the type word (cd ea / cf ea). block[0] = LINK,
 * block[1] = SEGMENT, block[2:4] = LENGTH (BE), block[4] = OPCODE. For the group
 * map: block[6] is the console byte and block[7:17] the group map itself —
 * 0x41 per input group from the front, 0xc3 per non-input group from the back.
 *
 * usage: group_map_scan FILE.pcap [FILE.pcap ...]
 */
#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/pcap_source.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define TYPE_OFF  16                    /* cd ea / cf ea, just before the block */
#define BLK_OFF   REAC_CTRL_BLOCK_OFF   /* 18 */

#define NT 32
#define NK 24
#define NS 32

struct kind { uint8_t link, op; uint16_t len; unsigned long n; };

struct talker {
	uint8_t mac[6];
	unsigned long frames, cdea, cfea, other;
	struct kind k[NK];
	int nk;
	/* cfea announce fields: [17] slot total, [18] box in-width, [19] pace, [20:22] boxes */
	uint8_t cfea_w[NS], cfea_pace[NS], cfea_boxes[NS];
	unsigned long cfea_n[NS];
	int ncfea;
};
static struct talker t[NT];
static int nt;

struct shape {
	uint8_t mac[6];
	uint8_t b[12];            /* block[5:17]: 04, console, then the 10 map bytes */
	unsigned long n;
	uint64_t first_us, last_us;
};
static struct shape sh[NS];
static int nsh;

/* chanmap section-marker family byte, counted over the whole scan */
static unsigned long chanmap_marker[256];

static struct talker *find(const uint8_t *mac)
{
	for (int i = 0; i < nt; i++)
		if (!memcmp(t[i].mac, mac, 6)) return &t[i];
	if (nt == NT) return NULL;
	memcpy(t[nt].mac, mac, 6);
	return &t[nt++];
}

static void note_kind(struct talker *k, uint8_t link, uint8_t op, uint16_t len)
{
	for (int i = 0; i < k->nk; i++)
		if (k->k[i].link == link && k->k[i].op == op && k->k[i].len == len) {
			k->k[i].n++; return;
		}
	if (k->nk == NK) return;
	k->k[k->nk++] = (struct kind){ link, op, len, 1 };
}

static void note_cfea(struct talker *k, uint8_t w, uint8_t pace, uint8_t boxes)
{
	for (int i = 0; i < k->ncfea; i++)
		if (k->cfea_w[i] == w && k->cfea_pace[i] == pace && k->cfea_boxes[i] == boxes) {
			k->cfea_n[i]++; return;
		}
	if (k->ncfea == NS) return;
	k->cfea_w[k->ncfea] = w; k->cfea_pace[k->ncfea] = pace;
	k->cfea_boxes[k->ncfea] = boxes; k->cfea_n[k->ncfea++] = 1;
}

static void note_shape(const uint8_t *mac, const uint8_t *b, uint64_t ts)
{
	for (int i = 0; i < nsh; i++)
		if (!memcmp(sh[i].mac, mac, 6) && !memcmp(sh[i].b, b, 12)) {
			sh[i].n++; sh[i].last_us = ts; return;
		}
	if (nsh == NS) return;
	memcpy(sh[nsh].mac, mac, 6);
	memcpy(sh[nsh].b, b, 12);
	sh[nsh].n = 1; sh[nsh].first_us = sh[nsh].last_us = ts;
	nsh++;
}

static void pmac(const uint8_t *m)
{
	printf("%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

int main(int argc, char **argv)
{
	int argi = 1, timeline = 0;
	if (argi < argc && !strcmp(argv[argi], "-t")) { timeline = 1; argi++; }
	if (argi >= argc) { fprintf(stderr, "usage: group_map_scan [-t] FILE.pcap [...]\n"); return 2; }
	static uint8_t buf[8192];
	unsigned long records = 0, reac = 0, vlan = 0, maps = 0;

	for (int a = argi; a < argc; a++) {
		struct pcap_source ps;
		if (pcap_source_open(&ps, argv[a]) != 0) {
			fprintf(stderr, "group_map_scan: cannot open %s\n", argv[a]);
			return 1;
		}
		for (;;) {
			uint64_t ts = 0;
			long n = pcap_source_next(&ps, buf, sizeof buf, &ts);
			if (n <= 0) break;
			records++;
			/* 802.1Q: strip the tag in place so the rest reads plain Ethernet.
			 * The corpus since 2026-09 is tagged; without this every frame below
			 * is invisible and the scan reports a clean, empty, WRONG result. */
			if (n > 18 && buf[12] == 0x81 && buf[13] == 0x00) {
				memmove(buf + 12, buf + 16, (size_t)n - 16);
				n -= 4;
				vlan++;
			}
			if (!reac_frame_is_reac(buf, (size_t)n)) continue;
			if (n < BLK_OFF + REAC_CTRL_BLOCK_LEN) continue;
			reac++;
			struct talker *k = find(buf + 6);
			if (!k) continue;
			k->frames++;
			const uint8_t *ty = buf + TYPE_OFF, *b = buf + BLK_OFF;
			if (ty[0] == 0xcd && ty[1] == 0xea) {
				k->cdea++;
				uint16_t len = (uint16_t)((b[2] << 8) | b[3]);
				note_kind(k, b[0], b[4], len);
				if (b[0] == 0x01 && b[4] == 0x10) {   /* THE GROUP MAP */
					note_shape(buf + 6, b + 5, ts);
					maps++;
					if (timeline) {
						/* the WHOLE 34-byte template (type word + block), so a
						 * golden can be built from the capture and not retyped */
						printf("%.3f GROUP_MAP  src=", (double)ts / 1e6);
						pmac(buf + 6);
						printf(" template=");
						for (int j = 0; j < 34; j++) printf("%02x", ty[j]);
						printf("\n");
					}
				}
				/* A BOX DECLARING ITSELF: link 1, opcode 0x8x, length 0x0010 —
				 * the config announce whose selector names the model (0x82 =
				 * S-1608 / 16 in, 0x84 = S-0808 or S-4000S). Printed so a group
				 * map can be read against WHICH box was on the wire when it was
				 * sent; without it, "the desk maps 8 for a 16-input box" cannot
				 * be told from "no 16-input box was there". */
				/* THE CHANMAP SWEEP (link 1, opcode 0x01, len 0x0019): eight
				 * 3-byte slots. The 0xfe slot is the section marker and its
				 * second byte is the console family (0x00 V-Mixer / 0x01 OHRCA)
				 * — the one byte of the sweep that is not console-independent,
				 * and the one a 44.1 kHz master has never been checked against. */
				if (b[0] == 0x01 && b[4] == 0x01 && len == 0x0019) {
					for (int sl = 0; sl < 8; sl++)
						if (b[5 + sl * 3] == 0xfe)
							chanmap_marker[b[5 + sl * 3 + 1]]++;
				}
				if (timeline && b[0] == 0x01 && (b[4] & 0x80) && len == 0x0010) {
					printf("%.3f BOX_ANNOUNCE src=", (double)ts / 1e6);
					pmac(buf + 6);
					printf(" selector=0x%02x cells=", b[4]);
					for (int j = 5; j < 20; j++) printf("%02x ", b[j]);
					printf("\n");
				}
			} else if (ty[0] == 0xcf && ty[1] == 0xea) {
				k->cfea++;
				int before = k->ncfea;
				note_cfea(k, b[16], b[17], b[19]);   /* [18],[19],[21] of the template */
				if (timeline && k->ncfea != before) {
					printf("%.3f CFEA_NEW   src=", (double)ts / 1e6);
					pmac(buf + 6);
					printf(" box_in_width=0x%02x pace=0x%02x boxes=%u\n",
					       b[16], b[17], b[19]);
				}
			} else {
				k->other++;
			}
		}
		pcap_source_close(&ps);
		printf("file=%s\n", argv[a]);
	}

	printf("records=%lu reac=%lu vlan_tagged=%lu group_maps=%lu\n",
	       records, reac, vlan, maps);
	if (!reac) {
		fprintf(stderr, "group_map_scan: no REAC frame — an empty scan is not a result\n");
		return 1;
	}
	for (int i = 0; i < nt; i++) {
		printf("talker "); pmac(t[i].mac);
		printf(" frames=%lu cdea=%lu cfea=%lu filler/other=%lu\n",
		       t[i].frames, t[i].cdea, t[i].cfea, t[i].other);
		for (int j = 0; j < t[i].nk; j++)
			printf("    ctrl link=%u op=0x%02x len=0x%04x n=%lu\n",
			       t[i].k[j].link, t[i].k[j].op, t[i].k[j].len, t[i].k[j].n);
		for (int j = 0; j < t[i].ncfea; j++)
			printf("    cfea box_in_width=0x%02x pace=0x%02x boxes=%u n=%lu\n",
			       t[i].cfea_w[j], t[i].cfea_pace[j], t[i].cfea_boxes[j], t[i].cfea_n[j]);
	}
	for (int i = 0; i < nsh; i++) {
		int in_groups = 0, out_groups = 0;
		for (int j = 2; j < 12; j++) {
			if (sh[i].b[j] == 0x41) in_groups++;
			if (sh[i].b[j] == 0xc3) out_groups++;
		}
		printf("group_map src="); pmac(sh[i].mac);
		printf(" console=0x%02x n=%lu inputs=%dx8=%d out_groups=%d bytes=",
		       sh[i].b[1], sh[i].n, in_groups, in_groups * 8, out_groups);
		for (int j = 0; j < 12; j++) printf("%02x ", sh[i].b[j]);
		printf("first=%.3f last=%.3f\n",
		       (double)sh[i].first_us / 1e6, (double)sh[i].last_us / 1e6);
	}
	for (int i = 0; i < 256; i++)
		if (chanmap_marker[i])
			printf("chanmap_marker family=0x%02x n=%lu\n", i, chanmap_marker[i]);
	if (!maps)
		printf("NO GROUP MAP IN THIS SCAN (the census above is the control)\n");
	return 0;
}
