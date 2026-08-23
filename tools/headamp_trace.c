// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Print every head-amp record in a capture, in wire order, with its direction.
 *
 * WHY THIS EXISTS AND WHY IT IS NOT corpus_check. corpus_check TALLIES — it
 * answers "did the library still decode this corpus". A granularity question is
 * the opposite shape: it needs the individual records in time order, with the
 * SOURCE MAC attached, because the whole discriminator is whether a channel
 * NOBODY WROTE changes in the box's own re-broadcast. A count cannot see that.
 *
 * It hand-rolls no byte math: reac_ctrl_parse classifies and fills CH/PARAM/
 * VALUE, reac_ctrl_headamp_record_verify checks the nested checksum, and
 * pcap_source hands over caplen AND origlen so a truncated record is labelled
 * rather than silently trusted.
 *
 * THE SNAPLEN TRAP IS LIVE HERE. The phantom capture ran at snaplen 200 and a
 * head-amp record lives at frame [32:40], well inside it — but that is a claim
 * to CHECK, not to assume, so every line prints cap/orig and the summary counts
 * how many head-amp records were short of REAC_CTRL_BLOCK_END.
 */

#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/pcap_source.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *param_name(uint8_t p)
{
	switch (p) {
	case REAC_HEADAMP_PHANTOM: return "phantom";
	case REAC_HEADAMP_PAD:     return "pad";
	case REAC_HEADAMP_SENS:    return "sens";
	default:                   return "param?";
	}
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: headamp_trace FILE.pcap\n");
		return 2;
	}
	struct pcap_source ps;
	if (pcap_source_open(&ps, argv[1]) != 0) {
		fprintf(stderr, "headamp_trace: cannot open %s\n", argv[1]);
		return 1;
	}

	static uint8_t buf[4096];
	uint64_t t0 = 0;
	unsigned long records = 0, reac = 0, ha = 0, ha_short = 0, ha_badck = 0;
	unsigned long frag = 0, grant = 0;
	for (;;) {
		uint64_t ts = 0;
		long n = pcap_source_next(&ps, buf, sizeof buf, &ts);
		if (n <= 0)
			break;
		records++;
		if (!t0)
			t0 = ts;
		if (!reac_frame_is_reac(buf, (size_t)n))
			continue;
		reac++;

		struct reac_ctrl_parsed p;
		enum reac_ctrl_kind k = reac_ctrl_parse(buf, (size_t)n, &p);
		if (k == REAC_CTRL_RECORD_FRAGMENT)
			frag++;
		if (k == REAC_CTRL_GRANT)
			grant++;
		if (k != REAC_CTRL_HEADAMP)
			continue;
		ha++;

		int truncated = ps.last_orig_len > (uint32_t)n;
		int short_of_block = (size_t)n < REAC_CTRL_BLOCK_END;
		if (short_of_block)
			ha_short++;
		int ck = reac_ctrl_headamp_record_verify(buf);
		if (ck != 0)
			ha_badck++;

		printf("%10.6f src=%02x:%02x:%02x:%02x:%02x:%02x "
		       "dst=%02x:%02x:%02x:%02x:%02x:%02x "
		       "ch=0x%02x param=%-7s value=0x%02x "
		       "seg=%u ctr=%u cap=%ld orig=%u%s%s%s\n",
		       (double)(ts - t0) / 1e6,
		       p.src[0], p.src[1], p.src[2], p.src[3], p.src[4], p.src[5],
		       p.dst[0], p.dst[1], p.dst[2], p.dst[3], p.dst[4], p.dst[5],
		       p.ch, param_name(p.param), p.value,
		       p.seg, p.counter, n, ps.last_orig_len,
		       truncated ? " TRUNC" : "",
		       short_of_block ? " SHORT-OF-BLOCK" : "",
		       ck ? " REC-CKSUM-BAD" : "");
	}
	pcap_source_close(&ps);

	/* A scan that found nothing looks exactly like a capture with no head-amp
	 * traffic in it. Say which, and fail on the empty one. */
	fprintf(stderr, "headamp_trace: records=%lu reac=%lu headamp=%lu "
	                "fragments=%lu grants=%lu short_of_block=%lu bad_record_cksum=%lu\n",
	        records, reac, ha, frag, grant, ha_short, ha_badck);
	if (!reac) {
		fprintf(stderr, "headamp_trace: no REAC frame decoded — an empty scan is not a result\n");
		return 1;
	}
	return 0;
}
