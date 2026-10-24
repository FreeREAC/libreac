// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libFuzzer harness: THE PCAP READER (src/pcap_source.c), the door every offline
 * capture comes in through before a frame decoder sees it.
 *
 * pcap_source_open() takes a path and fopen()s it. A harness may open no file but
 * its input, so this one is linked with -Wl,--wrap=fopen: every fopen() in the
 * program reaches __wrap_fopen below, which answers the one path FUZZ_PCAP_PATH
 * with fmemopen() over the input and hands every other path to the real fopen.
 * The library is untouched, and the reader runs its own header and record code
 * over exactly the bytes the fuzzer chose.
 *
 * Input:  [0]   the capacity of the record buffer, in units of 8 bytes (0..2040)
 *         [1..] the pcap file: global header, then records
 *
 * The record buffer is its own allocation of exactly that capacity, so the reader
 * filling a byte past it is a sanitizer report.
 *
 * Properties, each an abort():
 *   - open accepts exactly the two classic magics, and a reader that failed to open
 *     holds no stream;
 *   - a record handed back fits the caller's buffer, and reading stops (EOF or
 *     error) within one record per 16 bytes of file;
 *   - an 802.1Q tag is stripped exactly when the record carried one: the frame
 *     comes back four bytes shorter with the tag's VID on the source;
 *   - the timestamp is the record header's seconds and microseconds.
 */
#define _POSIX_C_SOURCE 200809L   /* fmemopen */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <reac/pcap_source.h>

#include "fuzz.h"

#define FUZZ_PCAP_PATH "<fuzz-input>"

static const uint8_t *g_file;
static size_t g_file_len;

FILE *__real_fopen(const char *path, const char *mode);
FILE *__wrap_fopen(const char *path, const char *mode);

FILE *__wrap_fopen(const char *path, const char *mode)
{
	if (path && strcmp(path, FUZZ_PCAP_PATH) == 0) {
		/* fmemopen refuses a zero-length buffer: an empty input cannot be opened,
		 * which is what an empty file's missing header earns it anyway. */
		if (g_file_len == 0)
			return NULL;
		return fmemopen((void *)g_file, g_file_len, mode);
	}
	return __real_fopen(path, mode);
}

static uint32_t rd32(const uint8_t *p, int swap)
{
	uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	             (uint32_t)p[3] << 24;
	return swap ? __builtin_bswap32(v) : v;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size >= FUZZ_INPUT_MAX)
		return 0;
	struct fuzz_in in = fuzz_in(data, size);
	size_t cap = (size_t)fuzz_u8(&in) * 8;
	g_file_len = fuzz_take(&in, in.n, &g_file);
	/* A private copy, so the reader works on bytes nothing else aliases. */
	uint8_t *file = fuzz_dup(g_file, g_file_len);
	g_file = file;

	struct pcap_source ps;
	memset(&ps, 0xa5, sizeof ps);
	int r = pcap_source_open(&ps, FUZZ_PCAP_PATH);
	uint32_t magic = g_file_len >= 4 ? rd32(file, 0) : 0;
	int good = g_file_len >= 24 && (magic == 0xA1B2C3D4u || magic == 0xD4C3B2A1u);
	if (r != (good ? 0 : -1))
		abort();
	if (r != 0) {
		if (ps.f != NULL)
			abort();
		free(file);
		return 0;
	}
	if (ps.swapped != (magic == 0xD4C3B2A1u))
		abort();

	uint8_t *buf = malloc(cap ? cap : 1);
	if (!buf)
		abort();
	/* The harness walks the same file alongside the reader to know what each record
	 * should come back as; it never decides anything the reader then has to agree to
	 * except the record the reader itself says it returned. */
	size_t at = 24;
	long n;
	size_t calls = 0;
	while ((n = pcap_source_next(&ps, buf, cap, NULL)) > 0) {
		if (++calls > g_file_len / 16 + 1)
			abort();   /* each record consumes at least its 16-byte header */
		if ((size_t)n > cap)
			abort();
		/* Skip the records the reader skipped: those larger than the buffer. */
		uint32_t incl;
		for (;;) {
			if (at + 16 > g_file_len)
				abort();   /* the reader returned a record the file does not hold */
			incl = rd32(file + at + 8, ps.swapped);
			if (incl <= cap)
				break;
			at += 16 + (size_t)incl;
		}
		const uint8_t *rec = file + at + 16;
		if (at + 16 + incl > g_file_len)
			abort();
		if (ps.last_orig_len != rd32(file + at + 12, ps.swapped))
			abort();
		int tagged = incl >= 18 && rec[12] == 0x81 && rec[13] == 0x00;
		if (ps.last_vlan_tagged != tagged)
			abort();
		if (tagged) {
			if ((size_t)n != incl - 4 || memcmp(buf, rec, 12) != 0 ||
			    memcmp(buf + 12, rec + 16, incl - 16) != 0 ||
			    ps.last_vlan_id != (((rec[14] << 8) | rec[15]) & 0x0fff))
				abort();
		} else if ((size_t)n != incl || memcmp(buf, rec, incl) != 0) {
			abort();
		}
		at += 16 + incl;
	}
	if (n < -1)
		abort();

	/* Once more from the top, with the timestamp asked for. */
	pcap_source_close(&ps);
	if (ps.f != NULL)
		abort();
	if (pcap_source_open(&ps, FUZZ_PCAP_PATH) != 0)
		abort();
	uint64_t ts = 0;
	if (g_file_len >= 24 + 16 && rd32(file + 24 + 8, ps.swapped) <= cap) {
		n = pcap_source_next(&ps, buf, cap, &ts);
		uint64_t want = (uint64_t)rd32(file + 24, ps.swapped) * 1000000ull +
		                rd32(file + 28, ps.swapped);
		if (n > 0 && ts != want)
			abort();
	}
	pcap_source_close(&ps);
	pcap_source_next(&ps, buf, cap, NULL);   /* a closed reader answers -1, safely */
	free(buf);
	free(file);
	return 0;
}
