// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* peer_replay — replay captured REAC traffic through the code that decides who a
 * segment answers to, one frame at a time, and write down what it decided.
 *
 * Built twice by tools/peer-replay.sh, once against the base libreac and once against
 * the tree under test, and run on the same pcaps. The two record sets are then compared
 * frame by frame by tools/peer-replay-compare.py. Nothing here knows which build it is:
 * it only uses API both have, so the difference between the two runs is the difference
 * the change made.
 *
 *   peer_replay replay <out-dir> <pcap>       every arm below, records into out-dir
 *   peer_replay attack <expect> <pcap> [dump] the forged-frame arm (expect: base|fixed)
 *
 * WHAT A REPLAY RUNS, per capture:
 *
 *   rx      the real reac_rx feeder thread (reac_rx_open/start on a pcap source). The
 *           reader and the ring are wrapped at link time (-Wl,--wrap), so the loop runs
 *           unchanged while each frame's outcome is read off its counters the moment the
 *           loop asks for the next one: decoded (with a hash of the samples it wrote),
 *           not ours, duplicate or bad. Run in the gate's modes: upstream unlocked,
 *           downstream unlocked, and upstream and downstream locked to each source the
 *           capture holds (reac_rx_peer_reset, as reac-pw and reac_tap lock it).
 *   master  the real pacer ingest (reac_pacer_rx_ingest) on a pacer that is not opened,
 *           as tests/test_peer_binding.c drives it, impersonating each MAC the boxes in
 *           the capture unicast to. Between frames the master is stepped one slot per
 *           250 us of capture time (reac_master_next), and what it would emit is built
 *           with reac_downstream_build + reac_master_stamp and digested.
 *   hunt    reac_hunt_observe on every frame and reac_hunt_step every 200 ms of capture
 *           time, the cadence reac-pw polls at, unpinned, pinned master and pinned slave.
 *
 * DISTILLED CAPTURES. reac-captures cuts most control frames to 50 bytes and keeps the
 * true length in the record's origlen. A cut record is put back to its wire length here:
 * zeros where the audio was, and the end marker where the frame ends. The control block
 * is all there. Both builds see the same bytes, so this cannot make a difference appear
 * or disappear; it only lets a length check see the length the wire carried.
 *
 * Records are fixed-size, one per pcap record, so record i of every file is frame i.
 * Every file starts with an 8-byte magic. */
#define _GNU_SOURCE
#include <reac/reac.h>
#include <reac/reac_ctrl.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_encode.h>
#include <reac/reac_hunt.h>
#include <reac/reac_ports.h>
#include <reac/reac_master.h>
#include <reac/reac_upstream.h>
#include <reac/pcap_source.h>
#include <reac/transport/reac_pacer.h>
#include <reac/transport/reac_ring.h>
#include <reac/transport/reac_rx.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUFSZ 4096
#define MAX_PEERS 6
#define US_SYNTH { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 }

static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* ---- reading a capture, with distilled records put back to wire length ---- */

static size_t restore(uint8_t *buf, size_t cap, long n, uint32_t orig)
{
	if (n <= 0)
		return 0;
	size_t len = (size_t)n;
	if (orig > len && len >= REAC_CTRL_BLOCK_OFF + REAC_CTRL_BLOCK_LEN && orig <= cap) {
		memset(buf + len, 0, orig - len);
		len = orig;
		size_t clean = reac_frame_clean_len(len);
		if (clean >= REAC_UPSTREAM_OVERHEAD) {
			buf[clean - 2] = REAC_END_MARKER_0;
			buf[clean - 1] = REAC_END_MARKER_1;
		}
	}
	return len;
}

long __real_pcap_source_next(struct pcap_source *ps, uint8_t *buf, size_t cap, uint64_t *ts);

struct frame { size_t len; uint64_t ts_us; int vlan; uint8_t b[BUFSZ]; };

static int next_frame(struct pcap_source *ps, struct frame *f)
{
	long n = __real_pcap_source_next(ps, f->b, BUFSZ, &f->ts_us);
	if (n <= 0)
		return (int)n;
	f->len = restore(f->b, BUFSZ, n, ps->last_orig_len);
	f->vlan = ps->last_vlan_tagged ? ps->last_vlan_id : -1;
	return 1;
}

/* ---- small helpers ---- */

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
	const uint8_t *b = p;
	for (size_t i = 0; i < n; i++)
		h = (h ^ b[i]) * 16777619u;
	return h;
}

static FILE *out_open(const char *dir, const char *name, const char *magic)
{
	char path[4096];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "peer_replay: %s: %s\n", path, strerror(errno));
		exit(2);
	}
	fwrite(magic, 8, 1, f);
	return f;
}

static void macstr(char *s, const uint8_t m[6])
{
	sprintf(s, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

struct macset { int n; uint8_t m[MAX_PEERS][6]; unsigned long cnt[MAX_PEERS]; };

static void macset_add(struct macset *s, const uint8_t m[6])
{
	for (int i = 0; i < s->n; i++)
		if (memcmp(s->m[i], m, 6) == 0) {
			s->cnt[i]++;
			return;
		}
	if (s->n < MAX_PEERS) {
		memcpy(s->m[s->n], m, 6);
		s->cnt[s->n++] = 1;
	}
}

static void pcap_hdr(FILE *f)
{
	uint32_t gh[6] = { 0xa1b2c3d4, 0x00040002, 0, 0, 65535, 1 };
	fwrite(gh, sizeof gh, 1, f);
}

static void pcap_rec(FILE *f, const uint8_t *b, size_t len, uint64_t ts_us)
{
	uint32_t rh[4] = { (uint32_t)(ts_us / 1000000u), (uint32_t)(ts_us % 1000000u),
	                   (uint32_t)len, (uint32_t)len };
	fwrite(rh, sizeof rh, 1, f);
	fwrite(b, len, 1, f);
}

/* The dumps keep one frame per distinct (length, type word, control block): the grammar
 * check needs every kind of frame the code took or built, not every copy of it. */
#define SEEN_BITS 20
static uint64_t seen[1u << SEEN_BITS];

static int first_sight(const uint8_t *b, size_t len)
{
	uint64_t k = 1469598103934665603ull ^ len;
	size_t end = len < REAC_CTRL_BLOCK_OFF + REAC_CTRL_BLOCK_LEN ? len
	                                                             : REAC_CTRL_BLOCK_OFF + REAC_CTRL_BLOCK_LEN;
	for (size_t i = 12; i < end; i++)
		if (i < 14 || i >= 16)            /* the counter does not make a frame new */
			k = (k ^ b[i]) * 1099511628211ull;
	if (len >= 12 && memcmp(b, BCAST, 6) == 0)
		k ^= 0x9e3779b97f4a7c15ull;
	k |= 1;
	uint32_t i = (uint32_t)(k >> (64 - SEEN_BITS));
	for (uint32_t n = 0; n < (1u << SEEN_BITS); n++, i = (i + 1) & ((1u << SEEN_BITS) - 1)) {
		if (seen[i] == k)
			return 0;
		if (!seen[i]) {
			seen[i] = k;
			return 1;
		}
	}
	return 0;
}

static FILE *g_accdump;

/* ---- the rx arm: reac_rx's own loop, observed through its reader and its ring ---- */

static struct {
	struct reac_rx *rx;
	FILE *rec;
	int have_prev;            /* a frame was handed out and its outcome is pending */
	uint64_t ok, other, dup, bad;
	uint32_t hash;            /* digest of what the pending frame wrote to the ring */
	int wrote;
	uint16_t prev_ctr;
	const uint8_t *prev_buf;
	size_t prev_len;
	uint64_t prev_ts;
	unsigned long frames;
	uint64_t n_ok, n_other, n_dup, n_bad, n_skip;
} W;

/* One record per frame: outcome, the counter it carried, and the ring digest. */
static void rx_settle(void)
{
	if (!W.have_prev)
		return;
	uint64_t ok = atomic_load(&W.rx->frames_ok), other = atomic_load(&W.rx->frames_other);
	uint64_t dup = atomic_load(&W.rx->frames_dup), bad = atomic_load(&W.rx->frames_bad);
	uint8_t out = 0;                       /* 0: never reached the gate (not REAC) */
	if (ok != W.ok) {
		out = 1;
		W.n_ok++;
		if (g_accdump && W.prev_buf && first_sight(W.prev_buf, W.prev_len))
			pcap_rec(g_accdump, W.prev_buf, W.prev_len, W.prev_ts);
	}
	else if (other != W.other) { out = 2; W.n_other++; }
	else if (dup != W.dup) { out = 3; W.n_dup++; }
	else if (bad != W.bad) { out = 4; W.n_bad++; }
	else W.n_skip++;
	W.ok = ok; W.other = other; W.dup = dup; W.bad = bad;
	uint8_t r[8] = { out, (uint8_t)W.prev_ctr, (uint8_t)(W.prev_ctr >> 8), 0 };
	uint32_t h = (out == 1 && W.wrote) ? W.hash : 0;
	memcpy(r + 4, &h, 4);
	fwrite(r, 8, 1, W.rec);
	W.have_prev = 0;
}

long __wrap_pcap_source_next(struct pcap_source *ps, uint8_t *buf, size_t cap, uint64_t *ts)
{
	rx_settle();
	if (!atomic_load(&W.rx->running))
		return 0;
	long n = __real_pcap_source_next(ps, buf, cap, ts);
	if (n <= 0) {
		/* One pass and no more: the loop reopens at EOF and then sees running == 0. */
		atomic_store(&W.rx->running, 0);
		return 0;
	}
	size_t len = restore(buf, cap, n, ps->last_orig_len);
	W.have_prev = 1;
	W.wrote = 0;
	W.hash = 2166136261u;
	W.prev_ctr = len >= 16 ? (uint16_t)(buf[14] | buf[15] << 8) : 0;
	W.prev_buf = buf;
	W.prev_len = len;
	W.prev_ts = ts ? *ts : 0;
	W.frames++;
	return (long)len;
}

uint32_t __wrap_reac_ring_write(struct reac_ring *r, const float *planar, uint32_t n,
                                uint32_t src_channels)
{
	(void)r;
	W.hash = fnv(W.hash, &src_channels, sizeof src_channels);
	W.hash = fnv(W.hash, planar, (size_t)n * src_channels * sizeof(float));
	W.wrote = 1;
	return n;
}

static void rx_run(const char *pcap, const char *dir, const char *name, enum reac_rx_accept acc,
                   const uint8_t *peer, FILE *sum)
{
	static struct reac_rx rx;
	static struct reac_ring ring;
	struct reac_rx_cfg cfg = { .kind = REAC_RX_PCAP, .source = pcap, .forced_rate = 48000,
	                           .pcap_realtime = 0, .accept = acc };
	memset(&W, 0, sizeof W);
	W.rx = &rx;
	W.rec = out_open(dir, name, "PRRX0001");
	if (reac_rx_open(&rx, &cfg, &ring) != 0) {
		fprintf(stderr, "peer_replay: reac_rx_open %s failed\n", pcap);
		exit(2);
	}
	if (peer)
		reac_rx_peer_reset(&rx, peer, 1);
	W.ok = W.other = W.dup = W.bad = 0;
	if (reac_rx_start(&rx) != 0) {
		fprintf(stderr, "peer_replay: reac_rx_start failed\n");
		exit(2);
	}
	pthread_join(rx.thread, NULL);
	rx_settle();
	reac_ring_free(&ring);
	fclose(W.rec);
	fprintf(sum, "%s frames=%lu ok=%llu other=%llu dup=%llu bad=%llu skip=%llu\n", name,
	        W.frames, (unsigned long long)W.n_ok, (unsigned long long)W.n_other,
	        (unsigned long long)W.n_dup, (unsigned long long)W.n_bad,
	        (unsigned long long)W.n_skip);
}

/* ---- the master arm ---- */

#define SLOT_NS 250000ull          /* 4000 frames/s, the 48 kHz cadence */
#define MAX_GAP_SLOTS 40000u       /* 10 s: a longer gap is replayed as 10 s */

struct emitdump { FILE *f; unsigned n, max; };

/* One slot: the FSM steps, and the frame it would put on the wire is built the way the
 * pacer builds it (silent audio, counter, control block) and folded into `h`. */
static uint32_t master_slot(struct reac_pacer *p, uint32_t h, struct emitdump *d,
                            uint64_t ts_us)
{
	static uint8_t fr[REAC_FRAME_BYTES];
	static float silent[REAC_SAMPLES_PER_PKT];
	static float *planar[REAC_MAX_CHANNELS];
	if (!planar[0])
		for (int i = 0; i < REAC_MAX_CHANNELS; i++)
			planar[i] = silent;
	uint16_t counter;
	int tmpl;
	enum reac_master_emit emit = reac_master_next(&p->master, &counter, &tmpl);
	if (emit == REAC_M_EMIT_FILLER)
		return fnv(h, &emit, 1);
	reac_downstream_build(fr, planar, REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT, counter, p->src);
	reac_master_stamp(&p->master, fr, emit, tmpl);
	if (d && d->f && d->n < d->max && first_sight(fr, sizeof fr)) {
		pcap_rec(d->f, fr, sizeof fr, ts_us);
		d->n++;
	}
	return fnv(h, fr + 16, REAC_CTRL_BLOCK_OFF + REAC_CTRL_BLOCK_LEN - 16);
}

static void master_run(const char *pcap, const char *dir, const char *name, const uint8_t us[6],
                       FILE *sum, FILE *accdump, FILE *emitdump)
{
	struct pcap_source ps;
	static struct frame f;
	if (pcap_source_open(&ps, pcap) != 0)
		exit(2);
	struct reac_pacer *p = calloc(1, sizeof *p);
	if (!p)
		exit(2);
	memcpy(p->src, us, 6);
	struct reac_console_cfg idle = REAC_CONSOLE_CFG_IDLE;
	reac_master_init(&p->master, us, &idle, 4000);
	FILE *rec = out_open(dir, name, "PRMA0002");
	struct emitdump ed = { emitdump, 0, 20000 };
	uint64_t t_slot = 0;        /* capture time of the next slot, ns */
	unsigned long frames = 0, accepted = 0, established = 0, drops = 0, slots = 0;
	unsigned long binds = 0;
	uint8_t lastbox[6] = { 0 };
	enum reac_master_state prev = p->master.state;
	uint32_t eh = 2166136261u;
	int r;
	while ((r = next_frame(&ps, &f)) > 0) {
		uint64_t t = f.ts_us * 1000ull;
		if (!t_slot)
			t_slot = t;
		unsigned k = 0;
		while (t_slot <= t) {
			if (k < MAX_GAP_SLOTS) {
				eh = master_slot(p, eh, &ed, t_slot / 1000ull);
				slots++;
			}
			k++;
			t_slot += SLOT_NS;
		}
		if (k > MAX_GAP_SLOTS)
			t_slot = t + SLOT_NS;
		uint64_t before = atomic_load(&p->rx_box_frames);
		/* what the frame is to the master, and what the master held when it arrived */
		uint8_t evb = 0xff;
		{
			struct reac_ctrl_parsed pr;
			enum reac_master_rx_event ev;
			if (f.len >= 52 && reac_ctrl_classify_box_frame(f.b, f.len, us, &pr, &ev) == 0)
				evb = (uint8_t)ev;
		}
		uint8_t st_before = (uint8_t)p->master.state, box_before[6];
		memcpy(box_before, p->master.box_mac, 6);
		if (memcmp(f.b + 6, us, 6) != 0 && f.len >= 14)
			reac_pacer_rx_ingest(p, f.b, f.len);
		int acc = atomic_load(&p->rx_box_frames) != before;
		if (acc) {
			accepted++;
			if (accdump && first_sight(f.b, f.len))
				pcap_rec(accdump, f.b, f.len, f.ts_us);
		}
		enum reac_master_state st = p->master.state;
		if (st == REAC_M_ESTABLISHED && prev != REAC_M_ESTABLISHED)
			established++;
		if (st < prev && prev >= REAC_M_GRANTING)
			drops++;
		if (st >= REAC_M_GRANTING && memcmp(lastbox, p->master.box_mac, 6) != 0) {
			binds++;
			memcpy(lastbox, p->master.box_mac, 6);
		}
		prev = st;
		/* 24 bytes: state and event before, box before, then what the frame did */
		uint8_t rr[24] = { st_before, evb };
		memcpy(rr + 2, box_before, 6);
		rr[8] = (uint8_t)st;
		rr[9] = (uint8_t)acc;
		rr[10] = (uint8_t)p->master.alloc.width;
		rr[11] = (uint8_t)p->declared_in;
		rr[12] = (uint8_t)p->master.drop_reason;
		memcpy(rr + 13, p->master.box_mac, 6);
		rr[20] = (uint8_t)eh;
		rr[21] = (uint8_t)(eh >> 8);
		fwrite(rr, sizeof rr, 1, rec);
		eh = 2166136261u;
		frames++;
	}
	pcap_source_close(&ps);
	fclose(rec);
	char bm[13];
	macstr(bm, p->master.box_mac);
	fprintf(sum, "%s frames=%lu accepted=%lu slots=%lu established=%lu drops=%lu binds=%lu "
	        "final=%s width=%d box=%s emitted_ctrl=%u\n", name, frames, accepted, slots,
	        established, drops, binds, reac_master_state_name(p->master.state),
	        p->master.alloc.width, bm, ed.n);
	free(p);
}

/* ---- the hunt arm ---- */

#define HUNT_POLL_NS (200ull * 1000000ull)

static void hunt_run(const char *pcap, const char *dir, FILE *sum)
{
	struct pcap_source ps;
	static struct frame f;
	static const uint8_t us[6] = US_SYNTH;
	static const char *pin_name[3] = { "none", "master", "slave" };
	if (pcap_source_open(&ps, pcap) != 0)
		exit(2);
	static struct reac_hunt h[3];
	FILE *rec = out_open(dir, "hunt.bin", "PRHU0002");
	uint64_t next_step = 0;
	unsigned long frames = 0, changes[3] = { 0 };
	uint64_t first_ns = 0, decided_ns[3] = { 0 };
	enum reac_hunt_verdict last[3];
	int r, init = 0;
	while ((r = next_frame(&ps, &f)) > 0) {
		uint64_t t = f.ts_us * 1000ull;
		if (!init) {
			for (int i = 0; i < 3; i++) {
				reac_hunt_init(&h[i], us, t);
				if (i == 1) reac_hunt_pin(&h[i], REAC_ROLE_MASTER);
				if (i == 2) reac_hunt_pin(&h[i], REAC_ROLE_SLAVE);
				last[i] = h[i].verdict;
			}
			next_step = t;
			first_ns = t;
			init = 1;
		}
		while (next_step <= t) {
			for (int i = 0; i < 3; i++)
				reac_hunt_step(&h[i], next_step);
			next_step += HUNT_POLL_NS;
		}
		int observed = 0;
		for (int i = 0; i < 3; i++)
			observed = reac_hunt_observe(&h[i], f.b, f.len, t, NULL) >= 0;
		/* 32 bytes: per variant, verdict, rival kind, and the rival's MAC; then whether
		 * the frame was a sighting at all (the table counts only those) */
		uint8_t rr[32] = { 0 };
		rr[24] = (uint8_t)observed;
		for (int i = 0; i < 3; i++) {
			if (h[i].verdict != last[i]) {
				changes[i]++;
				if (!decided_ns[i] && h[i].verdict != REAC_HUNT_HUNTING)
					decided_ns[i] = t - first_ns + 1;
				last[i] = h[i].verdict;
			}
			rr[i * 8] = (uint8_t)h[i].verdict;
			rr[i * 8 + 1] = (uint8_t)h[i].arb.rival;
			if (h[i].arb.have_mac)
				memcpy(rr + i * 8 + 2, h[i].arb.mac, 6);
		}
		fwrite(rr, sizeof rr, 1, rec);
		frames++;
	}
	pcap_source_close(&ps);
	fclose(rec);
	for (int i = 0; i < 3; i++)
		fprintf(sum, "hunt-%s frames=%lu changes=%lu final=%s rival=%s first_decided_ms=%.1f\n",
		        pin_name[i], frames, changes[i], reac_hunt_verdict_name(h[i].verdict),
		        reac_rival_kind_name(h[i].arb.rival),
		        decided_ns[i] ? (double)(decided_ns[i] - 1) / 1e6 : -1.0);
}

/* ---- meta: what each frame is, for the comparison to explain a difference by ---- */

static void meta_run(const char *pcap, const char *dir, struct macset *usrc, struct macset *dsrc,
                     struct macset *udst)
{
	struct pcap_source ps;
	static struct frame f;
	if (pcap_source_open(&ps, pcap) != 0)
		exit(2);
	FILE *rec = out_open(dir, "meta.bin", "PRME0001");
	int r;
	while ((r = next_frame(&ps, &f)) > 0) {
		/* ts_us u64, src[6], dst[6], counter u16, len u16, type word [16:18], block[0..5] */
		uint8_t m[32] = { 0 };
		memcpy(m, &f.ts_us, 8);
		if (f.len >= 14) {
			memcpy(m + 8, f.b + 6, 6);
			memcpy(m + 14, f.b, 6);
		}
		if (f.len >= 16) {
			m[20] = f.b[14];
			m[21] = f.b[15];
		}
		uint16_t l = (uint16_t)f.len;
		memcpy(m + 22, &l, 2);
		if (f.len >= 24)
			memcpy(m + 24, f.b + 16, 8);
		fwrite(m, 32, 1, rec);
		if (f.len < 52 || !reac_frame_is_reac(f.b, f.len))
			continue;
		size_t clean = reac_frame_clean_len(f.len);
		int bc = memcmp(f.b, BCAST, 6) == 0;
		if (!bc && reac_upstream_channels(clean) >= 0) {
			macset_add(usrc, f.b + 6);
			macset_add(udst, f.b);
		}
		if (bc && clean == REAC_FRAME_BYTES)
			macset_add(dsrc, f.b + 6);
		if (bc && reac_upstream_channels(clean) >= 0)
			macset_add(usrc, f.b + 6);   /* a box on M broadcasts its own geometry */
	}
	pcap_source_close(&ps);
	fclose(rec);
}

static int replay(const char *dir, const char *pcap)
{
	mkdir(dir, 0755);
	struct macset usrc = { 0 }, dsrc = { 0 }, udst = { 0 };
	meta_run(pcap, dir, &usrc, &dsrc, &udst);
	char path[4096];
	snprintf(path, sizeof path, "%s/summary.txt", dir);
	FILE *sum = fopen(path, "w");
	snprintf(path, sizeof path, "%s/accepted.pcap", dir);
	FILE *acc = fopen(path, "wb");
	snprintf(path, sizeof path, "%s/emitted.pcap", dir);
	FILE *emi = fopen(path, "wb");
	if (!sum || !acc || !emi)
		return 2;
	pcap_hdr(acc);
	pcap_hdr(emi);
	g_accdump = acc;
	char name[64], ms[13];

	rx_run(pcap, dir, "rx-up.bin", REAC_RX_ACCEPT_UPSTREAM, NULL, sum);
	rx_run(pcap, dir, "rx-down.bin", REAC_RX_ACCEPT_DOWNSTREAM, NULL, sum);
	for (int i = 0; i < usrc.n; i++) {
		macstr(ms, usrc.m[i]);
		snprintf(name, sizeof name, "rx-up-%s.bin", ms);
		rx_run(pcap, dir, name, REAC_RX_ACCEPT_UPSTREAM, usrc.m[i], sum);
	}
	for (int i = 0; i < dsrc.n; i++) {
		macstr(ms, dsrc.m[i]);
		snprintf(name, sizeof name, "rx-down-%s.bin", ms);
		rx_run(pcap, dir, name, REAC_RX_ACCEPT_DOWNSTREAM, dsrc.m[i], sum);
	}
	/* The master impersonates every MAC a box unicasts to, and a MAC nobody uses. */
	static const uint8_t synth[6] = US_SYNTH;
	for (int i = 0; i <= udst.n; i++) {
		const uint8_t *us = i < udst.n ? udst.m[i] : synth;
		macstr(ms, us);
		snprintf(name, sizeof name, "master-%s.bin", ms);
		master_run(pcap, dir, name, us, sum, acc, emi);
	}
	hunt_run(pcap, dir, sum);
	fclose(sum);
	fclose(acc);
	fclose(emi);
	return 0;
}

/* ======================================================================================
 * THE ATTACK ARM. Frames taken from a real capture and changed into what the security
 * findings M5, M6 and M8 (an unbound master, one forged role announce, audio sent as the
 * box) describe. Each arm prints ACCEPTED or REFUSED and checks it against what the
 * finding says the base library does and what the change must do.
 * ====================================================================================== */

static int afails;
static int expect_fixed;
static FILE *adump;

static void verdict(const char *id, int accepted, int base_accepts, int fixed_accepts)
{
	int want = expect_fixed ? fixed_accepts : base_accepts;
	int ok = (accepted != 0) == (want != 0);
	if (!ok)
		afails++;
	printf("  %-46s %-8s (%s expects %s)%s\n", id, accepted ? "ACCEPTED" : "REFUSED",
	       expect_fixed ? "fixed" : "base", want ? "ACCEPTED" : "REFUSED", ok ? "" : "  <-- RED");
}

static void dumpf(const uint8_t *b, size_t len)
{
	if (adump)
		pcap_rec(adump, b, len, 0);
}

/* Frames worth forging, taken from the capture. */
struct seeds {
	uint8_t box[6], us[6];
	int have_box;
	struct frame join, cfg, hb, uni;            /* the box's own frames */
	int have_join, have_cfg, have_hb, have_uni;
	struct frame cfea;                          /* a master announce, any source */
	int have_cfea;
	uint8_t cfea_src[6];
	struct frame up[64];                        /* consecutive returns of one box */
	int n_up;
};

static void seed(const char *pcap, struct seeds *s)
{
	struct pcap_source ps;
	static struct frame f;
	memset(s, 0, sizeof *s);
	if (pcap_source_open(&ps, pcap) != 0)
		exit(2);
	/* First pass: the box is whoever sends a config announce (it is unicast to its master,
	 * which names the master too), and its JOIN if the capture holds one. */
	while (next_frame(&ps, &f) > 0) {
		struct reac_ctrl_parsed pr;
		if (f.len < 52 || f.b[6] != 0x00 || f.b[7] != 0x40 || f.b[8] != 0xab)
			continue;
		enum reac_ctrl_kind k = reac_ctrl_parse(f.b, f.len, &pr);
		if (k == REAC_CTRL_FILLER || reac_ctrl_checksum_verify(f.b) != 0)
			continue;
		size_t clean = reac_frame_clean_len(f.len);
		/* A master announce; one at a box width (a box on M) is preferred, since that
		 * is the frame M6 is about. */
		if (k == REAC_CTRL_MASTER_ANNOUNCE &&
		    (!s->have_cfea || (clean < REAC_FRAME_BYTES && reac_frame_clean_len(s->cfea.len) >= REAC_FRAME_BYTES))) {
			s->cfea = f;
			memcpy(s->cfea_src, f.b + 6, 6);
			s->have_cfea = 1;
		}
		if (k == REAC_CTRL_CONFIG_ANNOUNCE && !s->have_cfg && !pr.is_broadcast) {
			s->cfg = f;
			s->have_cfg = 1;
			memcpy(s->box, f.b + 6, 6);
			memcpy(s->us, f.b, 6);
			s->have_box = 1;
		}
	}
	pcap_source_close(&ps);
	pcap_source_close(&ps);
	if (!s->have_box)
		return;
	/* Second pass: that box's heartbeat and its unicast returns to its master. */
	pcap_source_open(&ps, pcap);
	while (next_frame(&ps, &f) > 0) {
		struct reac_ctrl_parsed pr;
		enum reac_master_rx_event ev;
		if (f.len < 52 || memcmp(f.b + 6, s->box, 6) != 0)
			continue;
		if (reac_ctrl_classify_box_frame(f.b, f.len, s->us, &pr, &ev) != 0)
			continue;
		if (ev == REAC_M_RX_BOX_HEARTBEAT && !s->have_hb) { s->hb = f; s->have_hb = 1; }
		if (ev == REAC_M_RX_BOX_JOIN && !s->have_join) { s->join = f; s->have_join = 1; }
		size_t clean = reac_frame_clean_len(f.len);
		if (ev == REAC_M_RX_BOX_UNICAST && pr.kind == REAC_CTRL_FILLER &&
		    reac_upstream_channels(clean) >= 0 && s->n_up < 64) {
			if (!s->have_uni) { s->uni = f; s->have_uni = 1; }
			/* a run of returns that continue each other */
			if (s->n_up && (uint16_t)(reac_frame_counter(s->up[s->n_up - 1].b) + 1) !=
			    reac_frame_counter(f.b))
				s->n_up = 0;
			if (f.len == s->uni.len)
				s->up[s->n_up++] = f;
		}
	}
	pcap_source_close(&ps);
}

static const uint8_t EVIL[6] = { 0x00, 0x40, 0xab, 0x66, 0x66, 0x66 };

static void forge(uint8_t *b, const struct frame *src, const uint8_t mac[6])
{
	memcpy(b, src->b, src->len);
	memcpy(b + 6, mac, 6);
	reac_ctrl_checksum_apply(b);
}

static void slots(struct reac_pacer *p, int n)
{
	for (int i = 0; i < n; i++) {
		uint16_t c;
		int x;
		reac_master_next(&p->master, &c, &x);
	}
}

/* The captured box, through its own captured frames, to ESTABLISHED. */
static struct reac_pacer *established(const struct seeds *s)
{
	struct reac_pacer *p = calloc(1, sizeof *p);
	if (!p)
		exit(2);
	memcpy(p->src, s->us, 6);
	struct reac_console_cfg idle = REAC_CONSOLE_CFG_IDLE;
	reac_master_init(&p->master, s->us, &idle, 4000);
	static uint8_t b[BUFSZ];
	slots(p, 20000);
	for (int i = 0; i < 80000 && p->master.state != REAC_M_ESTABLISHED; i++) {
		slots(p, 1);
		if (i % 400 == 0) {
			memcpy(b, s->cfg.b, s->cfg.len);
			reac_pacer_rx_ingest(p, b, s->cfg.len);
			if (s->have_hb) {
				memcpy(b, s->hb.b, s->hb.len);
				reac_pacer_rx_ingest(p, b, s->hb.len);
			}
		}
	}
	return p;
}

/* Change one slot of the declared port table; v picks which. Returns 0 when this table
 * has no slot of the kind v changes. The checksum is put right after. */
#define TABLE_VARIANTS 4
static int table_edit(uint8_t *b, int v)
{
	static const struct { uint8_t from, to; int last; } e[TABLE_VARIANTS] = {
		{ 2, 3, 1 },    /* one input group fewer */
		{ 3, 2, 0 },    /* one input group more */
		{ 2, 1, 1 },    /* an input group becomes an output group */
		{ 1, 2, 0 },    /* an output group becomes an input group */
	};
	uint8_t *blk = b + REAC_CTRL_BLOCK_OFF;
	int at = -1;
	for (int i = REAC_PORTS_TABLE_OFF; i < REAC_PORTS_TABLE_OFF + REAC_PORTS_TABLE_SLOTS; i++)
		if (blk[i] == e[v].from && (e[v].last || at < 0))
			at = i;
	if (at < 0)
		return 0;
	blk[at] = e[v].to;
	reac_ctrl_checksum_apply(b);
	return 1;
}

static int attack_m5(const struct seeds *s)
{
	static uint8_t b[BUFSZ];
	if (!s->have_box || !s->have_cfg) {
		printf("  M5: no box config-announce in this capture, arm not run\n");
		return 0;
	}
	struct reac_pacer *p = established(s);
	if (p->master.state != REAC_M_ESTABLISHED) {
		printf("  M5: the captured box did not establish (state %s), arm not run\n",
		       reac_master_state_name(p->master.state));
		free(p);
		afails++;
		return 1;
	}
	int w0 = p->master.alloc.width;

	/* M5.a: the box's own config-announce, sent from another MAC, declaring another width.
	 * The port table: block[8..19] holds one code per group of four channels (reac_ports).
	 * Which edit is a declaration the master can place depends on the box (an S-1608 will
	 * not place one more input group), so the edit is the first of a short list that the
	 * BOX'S OWN MAC changes the declaration with, on this build. That is the control: the
	 * same bytes from the real box are taken, from EVIL they are not. */
	int variant = -1;
	for (int v = 0; v < TABLE_VARIANTS && variant < 0; v++) {
		struct reac_pacer *q = established(s);
		forge(b, &s->cfg, s->box);
		if (table_edit(b, v)) {
			int qd = q->declared_in;
			reac_pacer_rx_ingest(q, b, s->cfg.len);
			if (q->declared_in != qd)
				variant = v;
		}
		free(q);
	}
	if (variant < 0) {
		printf("  M5.a: no table edit the box's own MAC can make on this box, arm not run\n");
		afails++;
	} else {
		forge(b, &s->cfg, EVIL);
		table_edit(b, variant);
		dumpf(b, s->cfg.len);
		int d0 = p->declared_in;
		reac_pacer_rx_ingest(p, b, s->cfg.len);
		verdict("M5.a config-announce from another MAC", p->declared_in != d0 ||
		        p->master.alloc.width != w0, 1, 0);
	}

	/* M5.b: the box's heartbeat turned into a BYE (selector 0x81 -> 0x00), another MAC */
	if (s->have_hb) {
		forge(b, &s->hb, EVIL);
		b[REAC_CTRL_BLOCK_OFF + 4] = 0x00;
		reac_ctrl_checksum_apply(b);
		dumpf(b, s->hb.len);
		reac_pacer_rx_ingest(p, b, s->hb.len);
		verdict("M5.b BYE from another MAC", p->master.state != REAC_M_ESTABLISHED, 1, 0);
	}
	free(p);

	/* M5.c: the box's captured JOIN from another MAC, on an established session */
	if (s->have_join) {
		p = established(s);
		forge(b, &s->join, EVIL);
		dumpf(b, s->join.len);
		reac_pacer_rx_ingest(p, b, s->join.len);
		verdict("M5.c JOIN from another MAC (rebind)", p->master.state != REAC_M_ESTABLISHED ||
		        memcmp(p->master.box_mac, s->box, 6) != 0, 1, 0);
		free(p);
	} else {
		printf("  M5.c: no JOIN in this capture, arm not run\n");
	}

	/* M5.d: the box goes silent and another MAC sends its heartbeat; does the link live on? */
	if (s->have_hb) {
		p = established(s);
		forge(b, &s->hb, EVIL);
		int dropped = 0;
		for (int i = 0; i < 400000 && !dropped; i++) {
			slots(p, 1);
			if (i % 50 == 0) {
				uint8_t c[BUFSZ];
				memcpy(c, b, s->hb.len);
				reac_pacer_rx_ingest(p, c, s->hb.len);
			}
			dropped = p->master.state == REAC_M_PROBING;
		}
		verdict("M5.d another MAC keeps a silent box's link", !dropped, 1, 0);
		free(p);
	}

	/* M5.e: THE FORGED MAC ADOPTED AS THE BOX (the finding's third consequence): a JOIN
	 * event from another MAC handed straight to reac_master_rx, the door every caller of
	 * the FSM goes through, on an established link. The pacer's gate is not in front of it,
	 * so this is the master's own check on its own. */
	p = established(s);
	{
		uint8_t jb[32] = { 0 };
		if (s->have_join)
			memcpy(jb, s->join.b + REAC_CTRL_BLOCK_OFF, sizeof jb);
		reac_master_rx(&p->master, REAC_M_RX_BOX_JOIN, EVIL, jb);
	}
	verdict("M5.e JOIN event from another MAC, adopted",
	        p->master.state != REAC_M_ESTABLISHED || memcmp(p->master.box_mac, s->box, 6) != 0,
	        1, 0);
	free(p);

	/* M5.f: the BYE of M5.b handed to reac_master_rx the same way. */
	p = established(s);
	reac_master_rx(&p->master, REAC_M_RX_BOX_BYE, EVIL, NULL);
	verdict("M5.f BYE event from another MAC, master_rx",
	        p->master.state != REAC_M_ESTABLISHED, 1, 0);
	free(p);

	/* CONTROLS, both builds: the box's own BYE drops it; the box's own config holds it. */
	if (s->have_hb) {
		p = established(s);
		memcpy(b, s->hb.b, s->hb.len);
		b[REAC_CTRL_BLOCK_OFF + 4] = 0x00;
		reac_ctrl_checksum_apply(b);
		reac_pacer_rx_ingest(p, b, s->hb.len);
		verdict("control: the box's own BYE", p->master.state != REAC_M_ESTABLISHED, 1, 1);
		free(p);
	}
	p = established(s);
	verdict("control: the box itself establishes", p->master.state == REAC_M_ESTABLISHED, 1, 1);
	free(p);
	return 0;
}

/* The captured announce at a box's width: M6 is one cfea in a frame under 40 channels,
 * which the arbitration reads as a box on M. A desk's 1492 B announce keeps its header
 * and control block and is cut to an 8-channel frame with its end marker. */
static size_t cfea_len(const struct frame *src)
{
	size_t clean = reac_frame_clean_len(src->len);
	return clean < REAC_FRAME_BYTES ? src->len : REAC_UPSTREAM_OVERHEAD + 8 * REAC_UPSTREAM_BYTES_PER_CH;
}

static void cfea_frame(uint8_t *b, const struct frame *src, const uint8_t mac[6], uint16_t ctr)
{
	size_t n = cfea_len(src);
	memcpy(b, src->b, n);
	b[n - 2] = REAC_END_MARKER_0;
	b[n - 1] = REAC_END_MARKER_1;
	memcpy(b + 6, mac, 6);
	b[14] = (uint8_t)ctr;
	b[15] = (uint8_t)(ctr >> 8);
	reac_ctrl_checksum_apply(b);
}

static int attack_m6(const struct seeds *s)
{
	static uint8_t b[BUFSZ];
	static const uint8_t us[6] = US_SYNTH;
	if (!s->have_cfea) {
		printf("  M6: no cfea announce in this capture, arm not run\n");
		return 0;
	}
	const uint64_t t0 = 1000ull * 1000000ull;
	/* The captured announce, once, from a MAC that is not on the wire. */
	for (int pin = 0; pin < 2; pin++) {
		struct reac_hunt h;
		reac_hunt_init(&h, us, t0);
		if (pin)
			reac_hunt_pin(&h, REAC_ROLE_MASTER);
		cfea_frame(b, &s->cfea, EVIL, 1);
		if (pin == 0)
			dumpf(b, cfea_len(&s->cfea));
		reac_hunt_observe(&h, b, cfea_len(&s->cfea), t0, NULL);
		reac_hunt_step(&h, t0);
		reac_hunt_step(&h, t0 + 200ull * 1000000ull);
		int moved = h.verdict == REAC_HUNT_SLAVE || h.verdict == REAC_HUNT_REFUSED;
		verdict(pin ? "M6.a one forged cfea, pinned master" : "M6.b one forged cfea, unpinned",
		        moved, 1, 0);
		if (pin) {
			/* and after it, nothing: the pin drives */
			reac_hunt_step(&h, t0 + 1000ull * 1000000ull);
			verdict("M6.c pinned master drives once it is quiet",
			        h.verdict == REAC_HUNT_MASTER, 0, 1);
		}
	}
	/* CONTROL: the same announce streamed at wire cadence (a real master) is followed. */
	{
		struct reac_hunt h;
		reac_hunt_init(&h, us, t0);
		reac_hunt_pin(&h, REAC_ROLE_MASTER);
		for (int i = 0; i < 400; i++) {
			uint64_t t = t0 + (uint64_t)i * 250000ull;
			cfea_frame(b, &s->cfea, s->cfea_src, (uint16_t)i);
			reac_hunt_observe(&h, b, cfea_len(&s->cfea), t, NULL);
			if (i % 40 == 0)
				reac_hunt_step(&h, t);
		}
		reac_hunt_step(&h, t0 + 400ull * 250000ull);
		/* A box on M is joined by a pinned master; a desk outranks no pin. */
		int joined = h.verdict == REAC_HUNT_SLAVE;
		int box = h.arb.rival == REAC_RIVAL_BOX;
		printf("  control: the captured announcer streamed: rival=%s verdict=%s\n",
		       reac_rival_kind_name(h.arb.rival), reac_hunt_verdict_name(h.verdict));
		if (box)
			verdict("control: a streaming box on M is joined", joined, 1, 1);
	}
	return 0;
}

struct rxcounts { uint64_t ok, other, dup, bad; };

static struct rxcounts rx_file(const char *path, const uint8_t *peer)
{
	static struct reac_rx rx;
	static struct reac_ring ring;
	struct reac_rx_cfg cfg = { .kind = REAC_RX_PCAP, .source = path, .forced_rate = 48000,
	                           .accept = REAC_RX_ACCEPT_UPSTREAM };
	memset(&W, 0, sizeof W);
	W.rx = &rx;
	W.rec = fopen("/dev/null", "wb");
	struct rxcounts c = { 0 };
	if (reac_rx_open(&rx, &cfg, &ring) != 0)
		exit(2);
	if (peer)
		reac_rx_peer_reset(&rx, peer, 1);
	reac_rx_start(&rx);
	pthread_join(rx.thread, NULL);
	rx_settle();
	c.ok = W.n_ok; c.other = W.n_other; c.dup = W.n_dup; c.bad = W.n_bad;
	reac_ring_free(&ring);
	fclose(W.rec);
	return c;
}

static int attack_m8(const struct seeds *s, const char *tmpdir)
{
	if (s->n_up < 16) {
		printf("  M8: no run of 16 consecutive box returns in this capture, arm not run\n");
		return 0;
	}
	int n = s->n_up;
	char path[4096];
	static uint8_t b[BUFSZ];
	struct { const char *id; int kind; int base_acc; int fixed_acc; } arms[] = {
		{ "M8.a box MAC, counter 3 behind, other audio", 1, 1, 0 },
		{ "M8.b box MAC, the box's counter, other audio", 2, 1, 0 },
		{ "negative control: another MAC", 3, 0, 0 },
		{ "control: the box alone", 0, 1, 1 },
	};
	for (unsigned a = 0; a < sizeof arms / sizeof arms[0]; a++) {
		snprintf(path, sizeof path, "%s/m8-%u.pcap", tmpdir, a);
		FILE *f = fopen(path, "wb");
		if (!f)
			exit(2);
		pcap_hdr(f);
		for (int i = 0; i < n; i++) {
			pcap_rec(f, s->up[i].b, s->up[i].len, 0);
			if (arms[a].kind == 0)
				continue;
			memcpy(b, s->up[i].b, s->up[i].len);
			uint16_t c = reac_frame_counter(b);
			if (arms[a].kind == 1)
				c = (uint16_t)(c - 3);
			b[14] = (uint8_t)c;
			b[15] = (uint8_t)(c >> 8);
			for (size_t k = REAC_L2_HEADER_LEN; k + 2 < s->up[i].len - 2; k++)
				b[k] ^= 0x5a;                        /* forged samples */
			if (arms[a].kind == 3)
				memcpy(b + 6, EVIL, 6);
			pcap_rec(f, b, s->up[i].len, 0);
			if (i == 0)
				dumpf(b, s->up[i].len);
		}
		fclose(f);
		struct rxcounts c = rx_file(path, s->box);
		/* The forged frames are the odd records. Accepted means they were decoded: more
		 * frames decoded than the box sent. */
		int forged_decoded = c.ok > (uint64_t)n;
		int box_decoded = c.ok >= (uint64_t)n;
		printf("    ok=%llu other=%llu dup=%llu bad=%llu (box frames %d)\n",
		       (unsigned long long)c.ok, (unsigned long long)c.other,
		       (unsigned long long)c.dup, (unsigned long long)c.bad, n);
		if (arms[a].kind == 0)
			verdict(arms[a].id, box_decoded && c.other == 0, 1, 1);
		else
			verdict(arms[a].id, forged_decoded, arms[a].base_acc, arms[a].fixed_acc);
		unlink(path);
	}
	return 0;
}

static int attack(const char *expect, const char *pcap, const char *dump)
{
	expect_fixed = strcmp(expect, "fixed") == 0;
	if (!expect_fixed && strcmp(expect, "base") != 0) {
		fprintf(stderr, "peer_replay attack: expect base or fixed\n");
		return 2;
	}
	if (dump) {
		adump = fopen(dump, "wb");
		if (adump)
			pcap_hdr(adump);
	}
	static struct seeds s;
	seed(pcap, &s);
	char bm[13] = "-", um[13] = "-";
	if (s.have_box) {
		macstr(bm, s.box);
		macstr(um, s.us);
	}
	printf("%s: box=%s master=%s join=%d cfg=%d hb=%d returns=%d cfea=%d\n", pcap, bm, um,
	       s.have_join, s.have_cfg, s.have_hb, s.n_up, s.have_cfea);
	const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
	attack_m5(&s);
	attack_m6(&s);
	attack_m8(&s, tmp);
	if (adump)
		fclose(adump);
	printf("%s: %d attack check(s) red\n", pcap, afails);
	return afails ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc >= 4 && strcmp(argv[1], "replay") == 0)
		return replay(argv[2], argv[3]);
	if (argc >= 4 && strcmp(argv[1], "attack") == 0)
		return attack(argv[2], argv[3], argc >= 5 ? argv[4] : NULL);
	fprintf(stderr, "usage: peer_replay replay <out-dir> <pcap>\n"
	                "       peer_replay attack base|fixed <pcap> [forged.pcap]\n");
	return 2;
}
