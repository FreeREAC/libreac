// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <reac/transport/reac_ring.h>
#include <stdlib.h>
#include <string.h>
#ifdef REAC_RING_CHECK_SPSC
#include <pthread.h>
#endif

/* THE SINGLE-WRITER / SINGLE-READER INVARIANT IS CHECKED, not just documented.
 * Two writers corrupt `head` silently; two readers corrupt `tail`.
 *
 *   always on, RT-safe: one atomic exchange on entry and one store on exit per
 *   side. A second writer (or reader) that arrives while the first is inside the
 *   ring finds the flag set, is turned away (returns 0, nothing touched) and
 *   counted in `spsc_violations`. No lock, no log, no allocation, never aborts.
 *
 *   REAC_RING_CHECK_SPSC (test and fuzz builds only, never shipped): the first
 *   thread to write (read) is recorded and any other thread on that side is a
 *   violation even when the two never overlap in time, which is what a test
 *   needs to be deterministic. */
static int side_enter(_Atomic uint32_t *busy, _Atomic uint64_t *tid,
                      _Atomic uint64_t *violations)
{
	if (atomic_exchange_explicit(busy, 1, memory_order_acquire)) {
		atomic_fetch_add_explicit(violations, 1, memory_order_relaxed);
		return 0;
	}
#ifdef REAC_RING_CHECK_SPSC
	uint64_t me = (uint64_t)pthread_self(), none = 0;
	if (!atomic_compare_exchange_strong(tid, &none, me) && none != me) {
		atomic_store_explicit(busy, 0, memory_order_release);
		atomic_fetch_add_explicit(violations, 1, memory_order_relaxed);
		return 0;
	}
#else
	(void)tid;
#endif
	return 1;
}

static void side_leave(_Atomic uint32_t *busy)
{
	atomic_store_explicit(busy, 0, memory_order_release);
}

static uint32_t next_pow2(uint32_t v)
{
	if (v < 2)
		return 2;
	v--;
	v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
	return v + 1;
}

int reac_ring_init(struct reac_ring *r, uint32_t channels, uint32_t capacity_frames)
{
	uint32_t cap = next_pow2(capacity_frames);
	/* Defense-in-depth: reject a zero request or a rounding overflow. next_pow2
	 * wraps to 0 when the power-of-two ceiling exceeds UINT32_MAX; cap 0 would set
	 * mask = cap-1 = 0xFFFFFFFF over a 0-slot calloc (which glibc returns non-NULL
	 * for, so the !r->buf guard never fires) and the first write would scribble the
	 * heap. A garbage --rate reaches here as such a depth — fail cleanly. */
	if (capacity_frames == 0 || cap == 0)
		return -1;
	r->buf = calloc((size_t)cap * channels, sizeof(float));
	if (!r->buf)
		return -1;
	r->capacity = cap;
	r->mask = cap - 1;
	r->channels = channels;
	/* ZERO, not `channels`. This is what makes the calloc above load-bearing: a
	 * ring whose first write is narrow retires nothing, so the silence in the rows
	 * above that width is the allocator's, and a malloc here would put heap junk
	 * into the graph. Initialising this to `channels` instead would paper over
	 * that by memsetting the whole buffer on the first write -- pointless work,
	 * and it would hide the malloc bug from the test that looks for it. */
	r->wrote_channels = 0;
	atomic_store_explicit(&r->head, 0, memory_order_relaxed);
	atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
	atomic_store_explicit(&r->underruns, 0, memory_order_relaxed);
	atomic_store_explicit(&r->overruns, 0, memory_order_relaxed);
	return 0;
}

void reac_ring_free(struct reac_ring *r)
{
	free(r->buf);
	r->buf = NULL;
}

uint32_t reac_ring_readable(const struct reac_ring *r)
{
	uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
	uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
	return (h - t) & r->mask;
}

uint32_t reac_ring_writable(const struct reac_ring *r)
{
	/* leave one slot empty to disambiguate full from empty */
	return r->mask - reac_ring_readable(r);
}

static uint32_t ring_write(struct reac_ring *r, const float *planar, uint32_t n,
                           uint32_t src_channels)
{
	if (src_channels > r->channels)
		src_channels = r->channels;
	if (src_channels < r->wrote_channels) {
		/* The source narrowed. Rows [src_channels, wrote_channels) will no longer
		 * be written, so retire their contents now -- otherwise the previous box's
		 * audio loops in them for as long as the ring lives. Off the sample path:
		 * this runs on a width change, not per frame. */
		for (uint32_t c = src_channels; c < r->wrote_channels; c++)
			memset(r->buf + (size_t)c * r->capacity, 0,
			       (size_t)r->capacity * sizeof(float));
	}
	r->wrote_channels = src_channels;

	/* `n` is BOTH the count to write AND the per-channel stride of the source:
	 * planar[c*n + s]. The two must never diverge — clamping `n` to fit the ring
	 * would shrink the stride too and make every channel c>0 read the wrong source
	 * samples (silent cross-channel corruption). So keep the real stride and let
	 * the overrun path below bound how many we actually write. A single write of
	 * n > capacity can't fully fit; that's an overrun, handled like a transient
	 * full ring (short write, drop the newest remainder). Callers are expected to
	 * keep n <= ring capacity (the REAC quantum is 12). */
	const uint32_t stride = n;

	uint32_t free_slots = reac_ring_writable(r);
	uint32_t w = n;
	if (w > free_slots) {
		/* Overrun (transient): the ring is full. SPSC discipline forbids the
		 * PRODUCER from moving `tail` (only the consumer may) — so drop the
		 * NEWEST samples that don't fit here rather than racing the consumer to
		 * drop the oldest. Overrun should be rare: the resampler holds the fill
		 * near target, so this is only a safety net. (Bounded-latency trimming of
		 * the OLDEST, if wanted, belongs on the consumer side where moving `tail`
		 * is legal.) */
		atomic_fetch_add_explicit(&r->overruns, w - free_slots, memory_order_relaxed);
		w = free_slots;
		if (w == 0)
			return 0;
	}

	uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
	for (uint32_t c = 0; c < src_channels; c++) {
		float *slot = r->buf + (size_t)c * r->capacity;
		const float *src = planar + (size_t)c * stride;
		uint32_t pos = h;
		for (uint32_t s = 0; s < w; s++) {
			slot[pos] = src[s];
			pos = (pos + 1) & r->mask;
		}
	}
	atomic_store_explicit(&r->head, (h + w) & r->mask, memory_order_release);
	return w;
}

static uint32_t ring_read_planar(struct reac_ring *r, float *const *dst,
                                 uint32_t channels, uint32_t n)
{
	uint32_t avail = reac_ring_readable(r);
	uint32_t real = avail < n ? avail : n;
	uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
	uint32_t nch = channels < r->channels ? channels : r->channels;

	for (uint32_t c = 0; c < nch; c++) {
		const float *slot = r->buf + (size_t)c * r->capacity;
		float *out = dst[c];
		uint32_t pos = t;
		for (uint32_t s = 0; s < real; s++) {
			out[s] = slot[pos];
			pos = (pos + 1) & r->mask;
		}
		/* underrun: zero-fill the remainder so the DAC never gets garbage */
		for (uint32_t s = real; s < n; s++)
			out[s] = 0.0f;
	}
	atomic_store_explicit(&r->tail, (t + real) & r->mask, memory_order_release);
	if (real < n)
		atomic_fetch_add_explicit(&r->underruns, n - real, memory_order_relaxed);
	return real;
}

static uint32_t ring_trim(struct reac_ring *r, uint32_t keep)
{
	uint32_t avail = reac_ring_readable(r);
	if (avail <= keep)
		return 0;
	uint32_t drop = avail - keep;
	uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
	atomic_store_explicit(&r->tail, (t + drop) & r->mask, memory_order_release);
	return drop;
}

/* ---- the doors: refuse a NULL the caller handed us, enforce SPSC ---------- *
 * A refusal returns 0, moves neither head nor tail, writes nothing and counts in
 * `refused`; it never logs, allocates or aborts, so it is safe on the RT thread.
 * The ring cannot see how long a caller's planes are (n floats each, planar
 * holding src_channels * n) -- the length is the caller's own n -- so the door
 * checks what it can see: the pointers. */

uint32_t reac_ring_write(struct reac_ring *r, const float *planar, uint32_t n,
                         uint32_t src_channels)
{
	if (!r || !r->buf)
		return 0;
	if (n > 0 && src_channels > 0 && !planar) {
		atomic_fetch_add_explicit(&r->refused, 1, memory_order_relaxed);
		return 0;
	}
	if (!side_enter(&r->writer_busy, &r->writer_tid, &r->spsc_violations))
		return 0;
	uint32_t w = ring_write(r, planar, n, src_channels);
	side_leave(&r->writer_busy);
	return w;
}

/* A refused read still owes the caller silence: the caller publishes its planes
 * whatever the return, so leaving them as found would play stale memory. Zero the
 * planes that exist (n floats each, the caller's own n); a NULL plane or a NULL
 * dst has nothing to fill. */
static void refused_read_silence(float *const *dst, uint32_t nch, uint32_t n)
{
	if (!dst)
		return;
	for (uint32_t c = 0; c < nch; c++)
		if (dst[c])
			memset(dst[c], 0, (size_t)n * sizeof(float));
}

uint32_t reac_ring_read_planar(struct reac_ring *r, float *const *dst,
                               uint32_t channels, uint32_t n)
{
	if (!r || !r->buf) {
		refused_read_silence(dst, channels, n);
		return 0;
	}
	uint32_t nch = channels < r->channels ? channels : r->channels;
	if (n > 0 && nch > 0) {
		int bad = dst == NULL;
		for (uint32_t c = 0; !bad && c < nch; c++)
			bad = dst[c] == NULL;
		if (bad) {
			atomic_fetch_add_explicit(&r->refused, 1, memory_order_relaxed);
			refused_read_silence(dst, nch, n);
			return 0;
		}
	}
	if (!side_enter(&r->reader_busy, &r->reader_tid, &r->spsc_violations)) {
		refused_read_silence(dst, nch, n);
		return 0;
	}
	uint32_t got = ring_read_planar(r, dst, channels, n);
	side_leave(&r->reader_busy);
	return got;
}

uint32_t reac_ring_trim(struct reac_ring *r, uint32_t keep)
{
	if (!r || !r->buf)
		return 0;
	if (!side_enter(&r->reader_busy, &r->reader_tid, &r->spsc_violations))
		return 0;
	uint32_t d = ring_trim(r, keep);
	side_leave(&r->reader_busy);
	return d;
}
