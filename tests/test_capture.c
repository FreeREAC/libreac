// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test: the offline wire sources — pcap_source (the pcap reader every RE
 * workflow replays through) and reac_capture's unprivileged error contract.
 *
 * pcap_source round-trip: write a classic pcap (both native-LE and
 * byte-swapped per-packet headers) of known REAC-shaped frames, read it back
 * through the real pcap_source and require byte-equal frames, correct
 * lengths, correct timestamps, EOF -> 0. Negatives: a bad magic and a
 * truncated global header refuse to open; a truncated record body errors; an
 * oversized record is SKIPPED (not fatal) and the following frame still
 * arrives — the documented multi-thousand-frame-capture behavior.
 *
 * reac_capture is the LIVE AF_PACKET reader (there is no pcap writer in
 * libreac); its open needs a real NIC + CAP_NET_RAW, so only the error
 * contract is testable here: open on a nonexistent interface fails with
 * fd = -1, and next/set_nonblock on a closed capture return -1.
 */
#define _DEFAULT_SOURCE   /* mkdtemp, truncate under -std=c11 */
#include "reac/pcap_source.h"
#include "reac/reac_capture.h"
#include "reac/reac.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

#define NFRAMES 5
#define FRAME_LEN 340   /* an S-0808 8-ch upstream return shape */

/* the i-th test frame: REAC-shaped (0x8819, LE counter), body i-keyed */
static void mk_frame(uint8_t *f, int i)
{
	memset(f, (uint8_t)(0xA0 + i), FRAME_LEN);
	f[12] = 0x88; f[13] = 0x19;
	f[14] = (uint8_t)i; f[15] = 0x00;
	f[FRAME_LEN - 2] = 0xC2; f[FRAME_LEN - 1] = 0xEA;
}

static void wr_u32(FILE *f, uint32_t v, int swap)
{
	if (swap)
		v = __builtin_bswap32(v);
	uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
	fwrite(b, 1, 4, f);
}

/* write a classic pcap: global header + NFRAMES records (+ optionally one
 * oversized record between frames 2 and 3). `swap` flips every header word,
 * modelling a capture written on the other endianness. */
static void write_pcap(const char *path, int swap, int with_oversized)
{
	FILE *f = fopen(path, "wb");
	wr_u32(f, 0xA1B2C3D4u, swap);   /* magic (swap => reader sees D4C3B2A1) */
	wr_u32(f, 0x00040002u, swap);   /* version 2.4 */
	wr_u32(f, 0, swap); wr_u32(f, 0, swap);
	wr_u32(f, 65535, swap);         /* snaplen */
	wr_u32(f, 1, swap);             /* linktype ethernet */
	uint8_t frame[FRAME_LEN];
	for (int i = 0; i < NFRAMES; i++) {
		if (with_oversized && i == 3) {
			/* a jumbo record larger than the reader's buffer: must be
			 * skipped with the stream still aligned for frame 3+ */
			static uint8_t jumbo[4096];
			memset(jumbo, 0x55, sizeof jumbo);
			wr_u32(f, 99, swap); wr_u32(f, 0, swap);
			wr_u32(f, sizeof jumbo, swap); wr_u32(f, sizeof jumbo, swap);
			fwrite(jumbo, 1, sizeof jumbo, f);
		}
		mk_frame(frame, i);
		wr_u32(f, (uint32_t)(100 + i), swap);      /* ts sec */
		wr_u32(f, (uint32_t)(1000 * i), swap);     /* ts usec */
		wr_u32(f, FRAME_LEN, swap);                /* incl_len */
		wr_u32(f, FRAME_LEN, swap);                /* orig_len */
		fwrite(frame, 1, FRAME_LEN, f);
	}
	fclose(f);
}

/* read `path` back and require the byte-equal frame sequence + timestamps */
static void check_roundtrip(const char *path, const char *tag)
{
	struct pcap_source ps;
	char msg[128];
	snprintf(msg, sizeof msg, "%s: open", tag);
	CHECK(pcap_source_open(&ps, path) == 0, msg);
	uint8_t buf[2048], want[FRAME_LEN];
	uint64_t ts = 0;
	for (int i = 0; i < NFRAMES; i++) {
		long n = pcap_source_next(&ps, buf, sizeof buf, &ts);
		snprintf(msg, sizeof msg, "%s: frame %d length", tag, i);
		CHECK(n == FRAME_LEN, msg);
		mk_frame(want, i);
		snprintf(msg, sizeof msg, "%s: frame %d byte-equal", tag, i);
		CHECK(n == FRAME_LEN && memcmp(buf, want, FRAME_LEN) == 0, msg);
		snprintf(msg, sizeof msg, "%s: frame %d timestamp", tag, i);
		CHECK(ts == (uint64_t)(100 + i) * 1000000ull + (uint64_t)(1000 * i), msg);
		snprintf(msg, sizeof msg, "%s: frame %d is REAC", tag, i);
		CHECK(reac_frame_is_reac(buf, (size_t)n) == 1, msg);
	}
	snprintf(msg, sizeof msg, "%s: EOF -> 0", tag);
	CHECK(pcap_source_next(&ps, buf, sizeof buf, NULL) == 0, msg);
	pcap_source_close(&ps);
}

int main(void)
{
	char dir[] = "/tmp/libreac-capXXXXXX";
	CHECK(mkdtemp(dir) != NULL, "mkdtemp");
	char p1[256], p2[256], p3[256], p4[256];
	snprintf(p1, sizeof p1, "%s/le.pcap", dir);
	snprintf(p2, sizeof p2, "%s/swapped.pcap", dir);
	snprintf(p3, sizeof p3, "%s/bad.pcap", dir);
	snprintf(p4, sizeof p4, "%s/jumbo.pcap", dir);

	/* 1. round-trip, native little-endian headers */
	write_pcap(p1, 0, 0);
	check_roundtrip(p1, "le");

	/* 2. round-trip, byte-swapped headers (the reader's swap path) */
	write_pcap(p2, 1, 0);
	check_roundtrip(p2, "swapped");

	/* 3. oversized record between frames: skipped, stream stays aligned */
	{
		write_pcap(p4, 0, 1);
		struct pcap_source ps;
		CHECK(pcap_source_open(&ps, p4) == 0, "jumbo: open");
		uint8_t buf[2048], want[FRAME_LEN];
		int got = 0;
		for (;;) {
			long n = pcap_source_next(&ps, buf, sizeof buf, NULL);
			if (n <= 0) {
				CHECK(n == 0, "jumbo: clean EOF (skip is not an error)");
				break;
			}
			mk_frame(want, got);
			CHECK(n == FRAME_LEN && memcmp(buf, want, FRAME_LEN) == 0,
			      "jumbo: frames around the skip byte-equal, in order");
			got++;
		}
		CHECK(got == NFRAMES, "jumbo: all real frames delivered, jumbo dropped");
		pcap_source_close(&ps);
	}

	/* 3b. 802.1Q: a tagged frame built by inserting 81 00 00 0c into the
	 * untagged golden (VID 12, low 12 bits of the TCI) must read back as
	 * the SAME payload the untagged golden gives, plus the VID exposed on
	 * the source. Red on main: pcap_source hands the tag through untouched,
	 * so reac_frame_is_reac sees 0x8100 at [12:14] and calls it not-REAC. */
	{
		uint8_t want[FRAME_LEN];
		mk_frame(want, 0);
		uint8_t tagged[FRAME_LEN + 4];
		memcpy(tagged, want, 12);            /* dst + src, unchanged */
		tagged[12] = 0x81; tagged[13] = 0x00; /* 802.1Q EtherType */
		tagged[14] = 0x00; tagged[15] = 0x0c; /* TCI: prio 0, VID 12 */
		memcpy(tagged + 16, want + 12, FRAME_LEN - 12); /* inner ethertype + rest */

		char pv[256];
		snprintf(pv, sizeof pv, "%s/vlan.pcap", dir);
		FILE *f = fopen(pv, "wb");
		wr_u32(f, 0xA1B2C3D4u, 0);
		wr_u32(f, 0x00040002u, 0);
		wr_u32(f, 0, 0); wr_u32(f, 0, 0);
		wr_u32(f, 65535, 0);
		wr_u32(f, 1, 0);
		wr_u32(f, 500, 0); wr_u32(f, 0, 0);       /* ts */
		wr_u32(f, FRAME_LEN + 4, 0);               /* incl_len */
		wr_u32(f, FRAME_LEN + 4, 0);               /* orig_len */
		fwrite(tagged, 1, FRAME_LEN + 4, f);
		fclose(f);

		struct pcap_source ps;
		CHECK(pcap_source_open(&ps, pv) == 0, "vlan: open");
		uint8_t buf[2048];
		long n = pcap_source_next(&ps, buf, sizeof buf, NULL);
		CHECK(n == FRAME_LEN, "vlan: tag stripped, length == untagged golden");
		CHECK(n == FRAME_LEN && memcmp(buf, want, FRAME_LEN) == 0,
		      "vlan: payload byte-equal to the untagged golden once stripped");
		CHECK(ps.last_vlan_tagged == 1, "vlan: last_vlan_tagged set");
		CHECK(ps.last_vlan_id == 12, "vlan: VID == 12");
		CHECK(reac_frame_is_reac(buf, (size_t)n) == 1,
		      "vlan: stripped frame reads as REAC");
		pcap_source_close(&ps);
		unlink(pv);
	}

	/* 4. malformed: bad magic refuses to open */
	{
		FILE *f = fopen(p3, "wb");
		uint8_t junk[24];
		memset(junk, 0x42, sizeof junk);
		fwrite(junk, 1, sizeof junk, f);
		fclose(f);
		struct pcap_source ps;
		CHECK(pcap_source_open(&ps, p3) == -1, "bad magic -> open fails");
		CHECK(ps.f == NULL, "bad magic -> handle closed");
	}

	/* 5. malformed: truncated global header refuses to open */
	{
		FILE *f = fopen(p3, "wb");
		uint8_t magic[8] = { 0xd4, 0xc3, 0xb2, 0xa1, 0x02, 0x00, 0x04, 0x00 };
		fwrite(magic, 1, sizeof magic, f);   /* only 8 of the 24 bytes */
		fclose(f);
		struct pcap_source ps;
		CHECK(pcap_source_open(&ps, p3) == -1, "truncated global header -> open fails");
	}

	/* 6. malformed: a record body cut short mid-frame errors (-1, not 0) */
	{
		write_pcap(p3, 0, 0);
		CHECK(truncate(p3, 24 + 16 + FRAME_LEN + 16 + 100) == 0,   /* frame 1 body cut */
		      "truncated body: truncate");
		struct pcap_source ps;
		CHECK(pcap_source_open(&ps, p3) == 0, "truncated body: open ok");
		uint8_t buf[2048];
		CHECK(pcap_source_next(&ps, buf, sizeof buf, NULL) == FRAME_LEN,
		      "truncated body: frame 0 intact");
		CHECK(pcap_source_next(&ps, buf, sizeof buf, NULL) == -1,
		      "truncated body: cut frame -> -1");
		pcap_source_close(&ps);
	}

	/* 6b. a record too big for the buffer AND cut short inside its own body errors
	 *     (-1): the skip reads past it, and a short read is a cut record, exactly
	 *     as for a record that fits. It used to fseek, which succeeds past EOF, so
	 *     the cut capture ended as a clean EOF (0). */
	{
		write_pcap(p3, 0, 1);
		CHECK(truncate(p3, 24 + 3 * (16 + FRAME_LEN) + 16 + 100) == 0,
		      "cut jumbo: truncate");
		struct pcap_source ps;
		CHECK(pcap_source_open(&ps, p3) == 0, "cut jumbo: open ok");
		uint8_t buf[2048];
		for (int i = 0; i < 3; i++)
			CHECK(pcap_source_next(&ps, buf, sizeof buf, NULL) == FRAME_LEN,
			      "cut jumbo: frames before it intact");
		CHECK(pcap_source_next(&ps, buf, sizeof buf, NULL) == -1,
		      "cut jumbo: the cut oversized record -> -1, not EOF");
		pcap_source_close(&ps);
	}

	/* 7. nonexistent file refuses to open */
	{
		struct pcap_source ps;
		char nope[300];
		snprintf(nope, sizeof nope, "%s/does-not-exist.pcap", dir);
		CHECK(pcap_source_open(&ps, nope) == -1, "missing file -> open fails");
	}

	/* 8. reac_capture (live AF_PACKET): the unprivileged error contract */
	{
		struct reac_capture c;
		CHECK(reac_capture_open(&c, "libreac-no-such-nic0") == -1,
		      "capture: nonexistent ifname -> -1");
		CHECK(c.fd == -1, "capture: failed open leaves fd -1");
		uint8_t buf[64];
		CHECK(reac_capture_next(&c, buf, sizeof buf) == -1,
		      "capture: next on closed fd -> -1");
		CHECK(reac_capture_set_nonblock(&c, 1) == -1,
		      "capture: set_nonblock on closed fd -> -1");
		reac_capture_close(&c);   /* no-op on -1, must not crash */
	}

	/* cleanup */
	unlink(p1); unlink(p2); unlink(p3); unlink(p4);
	rmdir(dir);

	if (fails == 0)
		printf("OK: pcap_source round-trip (LE + swapped), jumbo skip, malformed "
		       "rejects; reac_capture unprivileged error contract\n");
	else
		printf("%d capture test(s) failed\n", fails);
	return fails ? 1 : 0;
}
