// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* REVIEW 2026-09-25, finding M6 (docs/audits/2026-09-25-libreac-review.md).
 *
 * reac_decode_plain_le() takes a caller's `struct reac_mode` and reads
 * (s*nch + ch)*3 past offset 50 with no check that the geometry fits the
 * 1440-byte region; its header says "minus the braid geometry checks (a linear
 * index needs none)". A linear index needs exactly the SIZE check reac_decode()
 * makes (`nch*ns*3 > REAC_AUDIO_BYTES` -> -1). With {48000, 42, 12} the last
 * read is at frame offset 50 + 503*3 + 2 = 1561 of a 1492-byte frame.
 *
 * The frame is placed so its last byte is the last byte of a readable page and
 * the next page is PROT_NONE; the call runs in a child, so an over-read shows as
 * SIGSEGV rather than as silently copied garbage. reac_decode() on the same
 * geometry is the control: it refuses (-1) without touching the guard page. */
#define _GNU_SOURCE
#include <reac/reac.h>
#include <reac/reac_decode.h>

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static uint8_t *guarded_frame(void)
{
	long pg = sysconf(_SC_PAGESIZE);
	uint8_t *base = mmap(NULL, (size_t)pg * 2, PROT_READ | PROT_WRITE,
	                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
		return NULL;
	if (mprotect(base + pg, (size_t)pg, PROT_NONE) != 0)
		return NULL;
	uint8_t *f = base + pg - REAC_FRAME_BYTES;
	memset(f, 0, REAC_FRAME_BYTES);
	f[12] = 0x88; f[13] = 0x19;
	f[REAC_FRAME_BYTES - 2] = REAC_END_MARKER_0;
	f[REAC_FRAME_BYTES - 1] = REAC_END_MARKER_1;
	return f;
}

/* Run fn in a child; return its exit code, or 128+signal. */
static int in_child(int (*fn)(void))
{
	pid_t pid = fork();
	if (pid == 0)
		_exit(fn());
	int st = 0;
	waitpid(pid, &st, 0);
	if (WIFSIGNALED(st))
		return 128 + WTERMSIG(st);
	return WEXITSTATUS(st);
}

static const struct reac_mode WIDE = { 48000, 42, 12 };
static uint8_t out[42 * 12 * 3];

static int braid_refuses(void)
{
	uint8_t *f = guarded_frame();
	return f && reac_decode(f, REAC_FRAME_BYTES, &WIDE, out) == -1 ? 0 : 1;
}

static int plain_refuses(void)
{
	uint8_t *f = guarded_frame();
	return f && reac_decode_plain_le(f, REAC_FRAME_BYTES, &WIDE, out) == -1 ? 0 : 1;
}

int main(void)
{
	int c = in_child(braid_refuses);
	if (c != 0) {
		printf("NOT A RESULT: test_review_decode_plain_le — the reac_decode control "
		       "did not refuse cleanly (child status %d)\n", c);
		return 2;
	}
	int v = in_child(plain_refuses);
	if (v != 0) {
		fprintf(stderr, "FAIL test_review_decode_plain_le: a 42x12 mode over a 1492 B "
		        "frame %s\n", v == 128 + SIGSEGV
		        ? "reads past the frame into the guard page (SIGSEGV)"
		        : "is decoded instead of refused");
		return 1;
	}
	printf("OK: test_review_decode_plain_le — an oversize geometry is refused\n");
	return 0;
}
