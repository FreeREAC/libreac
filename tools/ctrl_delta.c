// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Which control-block BYTES moved, and when, per talker and per message kind.
 *
 * The head-amp granularity question has an upstream half the console's own
 * state cannot answer: after a write to one channel, does the BOX report its
 * neighbours differently? The box does not necessarily answer in the same
 * record format it was written in, so looking only for head-amp records
 * assumes the answer. This instead takes the whole 32-byte control block of
 * every classified message, keys a stream on (src MAC, link, opcode, block
 * length), and prints every byte position that ever CHANGES within its stream,
 * with the time it changed.
 *
 * That makes absence checkable. If nothing in the box's own messages moves at
 * the moment of a write, that is a measurement, not a failure to look — but
 * only if the same instrument SEES the writes it is being asked about, which
 * is why the master's streams are printed by the same code and appear in the
 * same output. A probe that reports absence must first be shown to detect
 * presence.
 *
 * Byte 31 is the block checksum and byte-level counters ride in the frame
 * header, not the block; both are printed like anything else rather than
 * filtered, so the reader can see the instrument working.
 */

#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/pcap_source.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NSTREAM 64

struct stream {
	uint8_t  src[6];
	uint8_t  link, opcode, seg;
	uint16_t blk_len;
	uint8_t  kind;
	uint8_t  block[REAC_CTRL_BLOCK_LEN];
	int      seen;
	unsigned long frames;
	unsigned long changes[REAC_CTRL_BLOCK_LEN];
};

static struct stream st[NSTREAM];
static int nst;

static struct stream *find(const struct reac_ctrl_parsed *p, enum reac_ctrl_kind k)
{
	for (int i = 0; i < nst; i++)
		if (!memcmp(st[i].src, p->src, 6) && st[i].link == p->link &&
		    st[i].opcode == p->opcode && st[i].seg == p->seg &&
		    st[i].blk_len == p->blk_len && st[i].kind == (uint8_t)k)
			return &st[i];
	if (nst == NSTREAM)
		return NULL;
	struct stream *s = &st[nst++];
	memcpy(s->src, p->src, 6);
	s->link = p->link; s->opcode = p->opcode; s->seg = p->seg;
	s->blk_len = p->blk_len; s->kind = (uint8_t)k;
	return s;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: ctrl_delta FILE.pcap\n");
		return 2;
	}
	struct pcap_source ps;
	if (pcap_source_open(&ps, argv[1]) != 0) {
		fprintf(stderr, "ctrl_delta: cannot open %s\n", argv[1]);
		return 1;
	}

	static uint8_t buf[4096];
	uint64_t t0 = 0;
	unsigned long reac = 0, classified = 0, tooshort = 0;
	for (;;) {
		uint64_t ts = 0;
		long n = pcap_source_next(&ps, buf, sizeof buf, &ts);
		if (n <= 0)
			break;
		if (!t0)
			t0 = ts;
		if (!reac_frame_is_reac(buf, (size_t)n))
			continue;
		reac++;
		struct reac_ctrl_parsed p;
		enum reac_ctrl_kind k = reac_ctrl_parse(buf, (size_t)n, &p);
		if (k == REAC_CTRL_NONE || k == REAC_CTRL_FILLER)
			continue;
		if ((size_t)n < REAC_CTRL_BLOCK_END) { tooshort++; continue; }
		classified++;
		struct stream *s = find(&p, k);
		if (!s)
			continue;
		const uint8_t *blk = buf + REAC_CTRL_BLOCK_OFF;
		s->frames++;
		if (!s->seen) {
			s->seen = 1;
			memcpy(s->block, blk, REAC_CTRL_BLOCK_LEN);
			printf("%10.6f FIRST %s src=%02x:%02x:%02x:%02x:%02x:%02x "
			       "L%u.%u op=%02x len=%u block=",
			       (double)(ts - t0) / 1e6, reac_ctrl_kind_name(k),
			       p.src[0], p.src[1], p.src[2], p.src[3], p.src[4], p.src[5],
			       p.link, p.seg, p.opcode, p.blk_len);
			for (int i = 0; i < REAC_CTRL_BLOCK_LEN; i++)
				printf("%02x", blk[i]);
			printf("\n");
			continue;
		}
		for (int i = 0; i < REAC_CTRL_BLOCK_LEN; i++) {
			if (s->block[i] == blk[i])
				continue;
			s->changes[i]++;
			printf("%10.6f CHG   %s src=%02x:%02x:%02x:%02x:%02x:%02x "
			       "L%u.%u op=%02x byte[%02d] %02x -> %02x\n",
			       (double)(ts - t0) / 1e6, reac_ctrl_kind_name(k),
			       p.src[0], p.src[1], p.src[2], p.src[3], p.src[4], p.src[5],
			       p.link, p.seg, p.opcode, i, s->block[i], blk[i]);
			s->block[i] = blk[i];
		}
	}
	pcap_source_close(&ps);

	fprintf(stderr, "ctrl_delta: reac=%lu classified=%lu short_of_block=%lu streams=%d\n",
	        reac, classified, tooshort, nst);
	for (int i = 0; i < nst; i++) {
		fprintf(stderr, "  stream src=%02x:%02x:%02x:%02x:%02x:%02x L%u.%u op=%02x "
		                "len=%u frames=%lu changed_bytes=",
		        st[i].src[0], st[i].src[1], st[i].src[2],
		        st[i].src[3], st[i].src[4], st[i].src[5],
		        st[i].link, st[i].seg, st[i].opcode, st[i].blk_len, st[i].frames);
		int any = 0;
		for (int b = 0; b < REAC_CTRL_BLOCK_LEN; b++)
			if (st[i].changes[b]) { fprintf(stderr, "%s%d(%lu)", any++ ? "," : "", b, st[i].changes[b]); }
		fprintf(stderr, "%s\n", any ? "" : "none");
	}
	if (!classified) {
		fprintf(stderr, "ctrl_delta: nothing classified — an empty scan is not a result\n");
		return 1;
	}
	return 0;
}
