// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The ring's two RT entry points trusted their pointers. A NULL `planar` or a
 * NULL / NULL-holding `dst` was a crash on the realtime thread (reac-pw works
 * around the NULL plane by hand). The door now refuses: it returns 0, moves
 * neither head nor tail, writes nothing, and counts the refusal in `refused`.
 *
 * Each probe runs in a child so that a crash on the base is a RESULT (the
 * signal is printed), not the end of the test. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <reac/transport/reac_ring.h>

static int fails;
#define CHK(c) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* run probe(arg) in a child; return its exit status, or 100+signal if killed */
static int in_child(int (*probe)(void))
{
	fflush(NULL);
	pid_t p = fork();
	if (p == 0)
		_exit(probe());
	int st = 0;
	waitpid(p, &st, 0);
	return WIFSIGNALED(st) ? 100 + WTERMSIG(st) : WEXITSTATUS(st);
}

static struct reac_ring ring;
static float a[4 * 16], pl0[16], pl1[16], pl2[16], pl3[16];

static void fresh(void)
{
	if (ring.buf) reac_ring_free(&ring);
	memset(&ring, 0, sizeof ring);
	if (reac_ring_init(&ring, 4, 64) != 0) abort();
	for (int i = 0; i < 64; i++) a[i] = (float)i + 1;
}

/* Each probe returns 0 when refused cleanly with nothing moved, 1 otherwise. */
static int p_write_null(void)
{
	fresh();
	uint32_t w = reac_ring_write(&ring, NULL, 16, 4);
	return !(w == 0 && atomic_load(&ring.head) == 0 && ring.wrote_channels == 0 &&
	         atomic_load(&ring.refused) == 1);
}
static int p_read_null_dst(void)
{
	fresh();
	reac_ring_write(&ring, a, 16, 4);
	uint32_t got = reac_ring_read_planar(&ring, NULL, 4, 16);
	return !(got == 0 && atomic_load(&ring.tail) == 0 && atomic_load(&ring.refused) == 1);
}
static int p_read_null_plane(void)
{
	fresh();
	reac_ring_write(&ring, a, 16, 4);
	float *dst[4] = { pl0, pl1, NULL, pl3 };
	memset(pl0, 0x7f, sizeof pl0);
	uint32_t got = reac_ring_read_planar(&ring, dst, 4, 16);
	/* refused whole: not even plane 0 was written, tail did not move */
	return !(got == 0 && atomic_load(&ring.tail) == 0 &&
	         atomic_load(&ring.refused) == 1 && ((uint8_t *)pl0)[0] == 0x7f);
}
static void p_read_good(void)
{
	fresh();
	CHK(reac_ring_write(&ring, a, 16, 4) == 16);
	float *dst[4] = { pl0, pl1, pl2, pl3 };
	CHK(reac_ring_read_planar(&ring, dst, 4, 16) == 16);
	CHK(pl0[0] == 1.0f && pl1[0] == 17.0f && pl3[15] == 64.0f);
	CHK(atomic_load(&ring.refused) == 0);

}
/* channels narrower than the ring: planes past `channels` may be NULL */
static int p_read_narrow_ok(void)
{
	fresh();
	reac_ring_write(&ring, a, 16, 4);
	float *dst[4] = { pl0, pl1, NULL, NULL };
	return !(reac_ring_read_planar(&ring, dst, 2, 16) == 16 && atomic_load(&ring.refused) == 0);
}

int main(void)
{
	int r;
	r = in_child(p_write_null);       printf("write NULL planar      -> child %d\n", r); CHK(r == 0);
	r = in_child(p_read_null_dst);    printf("read NULL dst          -> child %d\n", r); CHK(r == 0);
	r = in_child(p_read_null_plane);  printf("read NULL plane        -> child %d\n", r); CHK(r == 0);
	r = in_child(p_read_narrow_ok);   printf("read narrow, tail NULL -> child %d\n", r); CHK(r == 0);
	p_read_good();
	return fails ? 1 : 0;
}
