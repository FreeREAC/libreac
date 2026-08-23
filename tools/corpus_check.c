// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Decode a REAC capture with libreac and print what came out of it.
 *
 * THE CAPTURES ARE A REGRESSION SUITE, NOT A DEMONSTRATION. The unit suite runs
 * on goldens: a change that makes the frames in front of you read better and
 * quietly stops decoding a capture that used to work leaves it green. So this
 * walks a whole pcap, pushes every frame through the library's own entry points
 * — reac_ctrl_parse, the two checksums, the head-amp record, the declared port
 * table, both audio decoders — and prints one deterministic line per file.
 * tools/run-corpus.sh diffs that against a committed baseline and refuses a
 * regression.
 *
 * Every number here is a COUNT OF LIBRARY CALLS AND THEIR RESULTS, so the line
 * moves when behaviour moves. A classification change moves it too, on purpose:
 * the baseline diff is the report.
 *
 * THE SNAPLEN TRAP. Several corpus captures were taken at snaplen 64/128/200/400
 * and a truncated record's length can land on 52+36n by coincidence, so it
 * reaches a frame decoder and fails there for a reason that has nothing to do
 * with the protocol. Every pcap record carries caplen AND origlen; this compares
 * them (pcap_source.last_orig_len) and counts truncated records apart. It does
 * NOT discard them: a snaplen of 50 or more still carries the entire control
 * block, which is most of what this checks, so those records are classified and
 * only the audio decode is skipped.
 */

#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_ports.h>
#include <reac/reac_decode.h>
#include <reac/reac_upstream.h>
#include <reac/pcap_source.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One more than the last kind. The names come from the library, so this file
 * carries no second spelling of them. */
#define NKINDS (REAC_CTRL_UNKNOWN_CTRL + 1)

/* The header triple (link, segment, opcode) — block[0], block[1], block[4] — read
 * straight off the wire, INDEPENDENT of how reac_ctrl_parse classifies it. This
 * is the positive control for a reclassification: the triples cannot move when
 * only the classifier does, so a baseline diff that changes a kind name while
 * every triple holds still is a relabelling and nothing more. It is also
 * checkable against a third party: firmware-protocol.md's corroboration table
 * counts the same triples over the same corpus. */
#define NTRIPLES 48
struct triples {
	unsigned key[NTRIPLES];        /* link<<16 | seg<<8 | opcode, +1 to mark used */
	unsigned long count[NTRIPLES];
	unsigned n;
};

static void triple_add(struct triples *tr, unsigned key)
{
	for (unsigned i = 0; i < tr->n; i++)
		if (tr->key[i] == key) { tr->count[i]++; return; }
	if (tr->n == NTRIPLES)
		return;                    /* the corpus holds 18; overflow would be news */
	tr->key[tr->n] = key;
	tr->count[tr->n++] = 1;
}

struct tally {
	unsigned long records, reac, truncated, off_length;
	struct triples tr;
	unsigned long kind[NKINDS];
	unsigned long cksum_ok, cksum_bad;
	unsigned long ha_rec_ok, ha_rec_bad;
	unsigned long ha_param[4];        /* 0 phantom, 1 pad, 2 sens, 3 other */
	unsigned long ha_sens_over;       /* SENS value past the 56-step table  */
	unsigned long ports_ok, ports_refused;
	unsigned long ports_in, ports_out;  /* last accepted declaration        */
	unsigned long ident_box;            /* matrix rows recognised           */
	unsigned long audio_dn_ok, audio_dn_bad;
	unsigned long audio_up_ok, audio_up_bad;
};

/* One frame. `corrupt` is the self-test: flip a control-block byte so a checker
 * that cannot fail is exposed before a green run is believed. */
static void feed(struct tally *t, uint8_t *f, size_t len, int truncated, int corrupt)
{
	if (!reac_frame_is_reac(f, len))
		return;
	t->reac++;
	if (corrupt)
		f[REAC_CTRL_BLOCK_OFF + 4] ^= 0xff;

	if (f[16] == 0xcd && f[17] == 0xea)
		triple_add(&t->tr, ((unsigned)f[18] << 16) |
		                   ((unsigned)f[19] << 8) | f[22]);

	struct reac_ctrl_parsed p;
	enum reac_ctrl_kind k = reac_ctrl_parse(f, len, &p);
	if ((int)k < NKINDS)
		t->kind[k]++;

	if (k != REAC_CTRL_NONE && k != REAC_CTRL_FILLER) {
		if (reac_ctrl_checksum_verify(f) == 0) t->cksum_ok++;
		else                                   t->cksum_bad++;
	}
	if (k == REAC_CTRL_HEADAMP) {
		if (reac_ctrl_headamp_record_verify(f) == 0) t->ha_rec_ok++;
		else                                        t->ha_rec_bad++;
		t->ha_param[p.param < 3 ? p.param : 3]++;
		if (p.param == REAC_HEADAMP_SENS && p.value > REAC_HEADAMP_SENS_MAX)
			t->ha_sens_over++;
	}
	if (reac_ctrl_identify_box(f, len))
		t->ident_box++;

	struct reac_box_ports bp;
	if (len >= REAC_CTRL_BLOCK_END &&
	    f[16] == 0xcd && f[17] == 0xea && f[18] == 0x01) {
		if (reac_ports_parse(f + REAC_CTRL_BLOCK_OFF, &bp) == 0) {
			t->ports_ok++;
			t->ports_in = (unsigned long)bp.in_ch;
			t->ports_out = (unsigned long)bp.out_ch;
		} else {
			t->ports_refused++;
		}
	}

	if (truncated)
		return;   /* the audio region is not there to decode */

	static uint8_t pcm[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION];
	size_t clean = reac_frame_clean_len(len);
	if (clean == REAC_FRAME_BYTES) {
		if (reac_decode(f, clean, &REAC_MODE_48K, pcm) == REAC_SAMPLES_PER_PKT)
			t->audio_dn_ok++;
		else
			t->audio_dn_bad++;
	} else if (reac_upstream_channels(len) > 0) {
		if (reac_upstream_decode(f, len, pcm) == REAC_SAMPLES_PER_PKT)
			t->audio_up_ok++;
		else
			t->audio_up_bad++;
	} else if (clean < REAC_UPSTREAM_OVERHEAD || (clean - REAC_UPSTREAM_OVERHEAD) % 36) {
		t->off_length++;
	}
}

static int scan(const char *path, const char *label, unsigned long cap, int corrupt)
{
	struct pcap_source ps;
	if (pcap_source_open(&ps, path) != 0) {
		fprintf(stderr, "corpus_check: cannot open %s\n", path);
		return -1;
	}
	struct tally t;
	memset(&t, 0, sizeof t);
	static uint8_t buf[2048];
	for (;;) {
		long n = pcap_source_next(&ps, buf, sizeof buf, NULL);
		if (n <= 0)
			break;
		t.records++;
		int truncated = ps.last_orig_len > (uint32_t)n;
		if (truncated)
			t.truncated++;
		feed(&t, buf, (size_t)n, truncated, corrupt);
		if (cap && t.records >= cap)
			break;
	}
	pcap_source_close(&ps);

	printf("%s records=%lu reac=%lu trunc=%lu offlen=%lu",
	       label, t.records, t.reac, t.truncated, t.off_length);
	for (int i = 0; i < NKINDS; i++)
		if (t.kind[i])
			printf(" %s=%lu", reac_ctrl_kind_name((enum reac_ctrl_kind)i),
			       t.kind[i]);
	for (unsigned a = 0; a < t.tr.n; a++) {    /* sorted, so the line is stable */
		unsigned best = 0;
		for (unsigned b = 1; b < t.tr.n; b++)
			if (t.tr.key[b] < t.tr.key[best]) best = b;
		printf(" L%u.%u.%02x=%lu", t.tr.key[best] >> 16,
		       (t.tr.key[best] >> 8) & 0xff, t.tr.key[best] & 0xff,
		       t.tr.count[best]);
		t.tr.key[best] = ~0u;
	}
	printf(" cksum=%lu/%lu", t.cksum_ok, t.cksum_ok + t.cksum_bad);
	if (t.ha_rec_ok + t.ha_rec_bad)
		printf(" ha_rec=%lu/%lu ha_p=%lu/%lu/%lu/%lu ha_sens_over=%lu",
		       t.ha_rec_ok, t.ha_rec_ok + t.ha_rec_bad,
		       t.ha_param[0], t.ha_param[1], t.ha_param[2], t.ha_param[3],
		       t.ha_sens_over);
	if (t.ports_ok + t.ports_refused)
		printf(" ports=%lu/%lu decl=%lux%lu",
		       t.ports_ok, t.ports_ok + t.ports_refused, t.ports_in, t.ports_out);
	if (t.ident_box)
		printf(" boxmatch=%lu", t.ident_box);
	if (t.audio_dn_ok + t.audio_dn_bad)
		printf(" dn=%lu/%lu", t.audio_dn_ok, t.audio_dn_ok + t.audio_dn_bad);
	if (t.audio_up_ok + t.audio_up_bad)
		printf(" up=%lu/%lu", t.audio_up_ok, t.audio_up_ok + t.audio_up_bad);
	printf("\n");
	return 0;
}

int main(int argc, char **argv)
{
	unsigned long cap = 1000000;
	int corrupt = 0, i = 1;
	const char *root = "";
	for (; i < argc; i++) {
		if (!strcmp(argv[i], "--per-file") && i + 1 < argc)
			cap = strtoul(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--self-test"))
			corrupt = 1;
		else if (!strcmp(argv[i], "--strip-prefix") && i + 1 < argc)
			root = argv[++i];
		else
			break;
	}
	if (i >= argc) {
		fprintf(stderr,
		        "usage: corpus_check [--per-file N] [--self-test] "
		        "[--strip-prefix DIR] FILE...\n");
		return 2;
	}
	size_t rl = strlen(root);
	int seen = 0, bad = 0;
	for (; i < argc; i++) {
		const char *label = argv[i];
		if (rl && !strncmp(label, root, rl))
			label += rl;
		while (*label == '/')
			label++;
		if (scan(argv[i], label, cap, corrupt) != 0)
			bad++;
		else
			seen++;
	}
	/* A scan that found nothing looks exactly like a clean corpus. Say so and
	 * fail rather than print an empty report over a wrong path. */
	if (!seen) {
		fprintf(stderr, "corpus_check: no capture decoded — an empty scan is not a pass\n");
		return 1;
	}
	fprintf(stderr, "corpus_check: %d captures decoded, %d unreadable\n", seen, bad);
	return bad ? 1 : 0;
}
