// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The plain driver of fuzz/records.sh: reads one input file whole and calls the
 * harness once on it, as the OSS Verify driver does for an input with no config
 * bytes. Exit 0 when the harness returns 0 or -1, 78 for any other return, and 2
 * when the input cannot be read; a failed check is the 134 of its abort(). */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	FILE *f = fopen(argv[1], "rb");
	if (!f)
		return 2;
	size_t cap = 4096, n = 0;
	uint8_t *buf = malloc(cap);
	if (!buf)
		return 2;
	for (;;) {
		n += fread(buf + n, 1, cap - n, f);
		if (n < cap)
			break;
		uint8_t *more = realloc(buf, cap * 2);
		if (!more)
			return 2;
		buf = more;
		cap *= 2;
	}
	if (ferror(f))
		return 2;
	fclose(f);
	int r = LLVMFuzzerTestOneInput(buf, n);
	free(buf);
	return r == 0 || r == -1 ? 0 : 78;
}
