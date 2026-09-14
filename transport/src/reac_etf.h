// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_etf — the ETF pacing backend: the kernel releases the frame, not the thread.
 *
 * WHAT CHANGES. The userspace pacer decides WHEN a frame leaves by being awake at
 * that instant: clock_nanosleep to an absolute deadline, then sendto. Every
 * scheduling tail between the wake and the syscall lands on the wire, and that
 * tail is measured — 3.6 late slots/s and 900 ppm of transmit deficit on the live
 * rig (reac_pacer.h, slot-debt comment). The ETF backend moves the release: the
 * socket carries SO_TXTIME, every frame carries a SCM_TXTIME LAUNCH TIME, and the
 * kernel's etf qdisc (or the NIC, where it offloads) holds the packet until that
 * instant. The thread only has to be EARLY, by a bounded lead — it no longer has
 * to be punctual.
 *
 * WHAT THAT BOUGHT ELSEWHERE. reac_repacer, the OpenWrt de-jitter relay, is the
 * prior art for all of this and measured the egress cadence tighten from 3.6 us to
 * 1.4 us of jitter when the same mechanism was switched on
 * (reac-aes67-split-src/docs/design/specs/2026-06-11-etf-localin-clock-recovery.md).
 * Its two hard-won laws are carried here rather than rediscovered: the grid is
 * ACCUMULATED and never re-based on `now` (substituting `deadline = now + period`
 * put that rig 526.7 ppm off the master where accumulating held it to 8.7 ppm —
 * reac-repacer/docs/internals.md), and a slot is ALWAYS filled, which this pacer
 * already does with its FILLER. Three things it did NOT do are done here, each
 * because its absence cost that project real time: the SO_TXTIME probe is checked,
 * the TAI offset is checked, and the qdisc is checked — `--etf` ran as a silent
 * no-op on a port whose qdisc was `noqueue`, stamping every frame and having every
 * stamp ignored (REAC-REPACE-MASTER-CLOCK-LIMIT.md, finding 1). Errors are also
 * armed and drained, which that daemon left off, so a frame the qdisc refuses is
 * counted instead of vanishing.
 *
 * ONE MECHANISM, A SELECTOR. This is not a second pacer. reac_pacer's loop, its
 * FSM step, its FILLER, its depth guard, its telemetry and its catch-up law are
 * all unchanged; only the emit and the wake target move. The backend is chosen at
 * open from REACPW_PACER through reac_conf's existing layers.
 *
 * THE LAUNCH GRID IS EXACT, AND THAT IS THE WHOLE POINT. A launch time is an
 * absolute instant, so a truncated period is not absorbed by the next slot the way
 * a relative sleep absorbs it — it ACCUMULATES. At 3675 fps the true period is
 * 272 108.8435… ns and reac_pacer_period_ns rounds it to 272 109: 0.157 ns of error
 * per slot — 575 ns/s, 34.5 µs/min, 2.07 ms/h, and a WHOLE SLOT PERIOD of phase
 * after about eight minutes (test_reac_etf measures the first of those: 5 750 ns
 * over a 10 s window). So the grid advances by an integer quotient plus an integer
 * remainder accumulator — never a truncation — exactly as the 44.1 kHz burst fix
 * did. launch(n) is floor(base + n·10^9/fps) to the nanosecond, for every n.
 *
 * CLOCK_TAI, NOT MONOTONIC. The etf qdisc compares against a system clock, and the
 * only one it can be told to use that is both absolute and free of leap-second
 * steps is CLOCK_TAI. A kernel whose TAI offset is zero (nothing has ever
 * disciplined it) reports a CLOCK_TAI that is really UTC, so every launch time we
 * compute is 37 s from where the qdisc will read it — the frames are dropped as
 * expired, or held for half a minute. That is refused at open, by code, rather
 * than discovered on the wire.
 *
 * Private to transport/src: nothing here is installed, so all of it is backend
 * state that no public struct grows for (the ABI layout ratchet, tests/abi-layout.inc). */
#ifndef REAC_ETF_H
#define REAC_ETF_H

#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <time.h>

/* ---- refusals, by CODE ---------------------------------------------------- *
 *
 * Every one of these is a precondition the operator can fix, and each is named so
 * the journal line says which. Never classified by a strerror string. */
enum reac_etf_refusal {
	REAC_ETF_OK = 0,
	REAC_ETF_REFUSE_NO_TXTIME,     /* setsockopt(SO_TXTIME) refused: kernel too old,
	                                * or the socket family does not carry it */
	REAC_ETF_REFUSE_NO_QDISC,      /* no etf qdisc on this netdev: every launch time
	                                * would be stamped and then ignored */
	REAC_ETF_REFUSE_TAI_UNSET,     /* the kernel's TAI offset is 0 — CLOCK_TAI is
	                                * really UTC and every launch time is 37 s wrong */
	REAC_ETF_REFUSE_NO_TAI_CLOCK,  /* clock_gettime(CLOCK_TAI) itself failed */
	REAC_ETF_REFUSE_BAD_FPS,       /* fps outside the closed pace list */
	REAC_ETF_REFUSE_BAD_LEAD,      /* the lead is not inside its bounds */
};

/* A short phrase for the code. Never NULL — a refusal an operator cannot read is a
 * refusal they will guess at. */
const char *reac_etf_refusal_name(enum reac_etf_refusal r);

/* ---- the exact launch grid ------------------------------------------------ *
 *
 * launch(n) = base_ns + floor(n · 10^9 / fps), computed by accumulation so that no
 * slot's error is ever thrown away. `whole` and `rem` are the quotient and
 * remainder of 10^9 / fps; `frac` carries the remainder forward and pays a whole
 * nanosecond back every time it reaches fps.
 *
 * 8000 fps: whole 125 000, rem 0      — every step exactly 125 000 ns.
 * 4000 fps: whole 250 000, rem 0      — every step exactly 250 000 ns.
 * 3675 fps: whole 272 108, rem 3 100  — steps of 272 108 and 272 109 ns in the
 *           ratio the true period demands; over 3675 slots exactly 10^9 ns. */
struct reac_etf_grid {
	uint64_t launch_ns;   /* the launch time of the slot about to be sent */
	uint64_t base_ns;     /* slot 0's launch time (CLOCK_TAI) */
	uint64_t slot;        /* how many slots have been advanced past base */
	uint32_t whole;       /* 10^9 / fps */
	uint32_t rem;         /* 10^9 % fps */
	uint32_t frac;        /* the running remainder, 0 <= frac < fps */
	uint32_t fps;
};

/* Start the grid at `base_ns` (a CLOCK_TAI instant). Returns REAC_ETF_REFUSE_BAD_FPS
 * for fps <= 0, and never divides by zero. */
enum reac_etf_refusal reac_etf_grid_init(struct reac_etf_grid *g, int fps, uint64_t base_ns);

/* Advance one slot and return the new launch time.
 *
 * `steer_ns` is 0 for the nominal grid — the exact accumulation above, which is what
 * a free-running master uses. A non-zero value is the period the clock discipline
 * has steered to for this slot (reac_pacer_clock_tick): the launch time advances by
 * exactly that integer instead, because the discipline is already the thing deciding
 * where the grid should be and a second correction underneath it would fight it.
 * The remainder accumulator is left untouched across a steered slot, so dropping
 * back to the nominal grid resumes where it left off rather than re-truncating. */
uint64_t reac_etf_grid_advance(struct reac_etf_grid *g, long steer_ns);

/* Re-base the grid onto `now_ns` + one period, keeping the remainder phase. Used by
 * the pacer's existing catch-up law when the debt exceeds its budget: the same
 * decision it already makes, applied to launch times instead of to a deadline. */
void reac_etf_grid_rebase(struct reac_etf_grid *g, uint64_t now_ns);

/* ---- the lead ------------------------------------------------------------- *
 *
 * How far ahead of a frame's launch time the thread hands it to the kernel. It is
 * applied to the WAKE, never to the stamp: the thread sleeps to (launch - lead),
 * submits, and the kernel owns the instant. reac_repacer does exactly this
 * (reac_repacer.c:1572, `wake = deadline - g_etf_lead_ns`) and it is the whole
 * shape of the backend — the thread stops having to be punctual and only has to be
 * early.
 *
 * WHAT IT MUST COVER, and where each number comes from:
 *
 *   the thread's worst wake tail   2000 us   MEASURED, this pacer, 30-minute soak:
 *                                            p50 250, p90 500, p95 750, worst 2000
 *                                            (reac_pacer.h, REAC_CATCHUP_MAX_DEFAULT_US)
 *   the qdisc's own `delta`         300 us   the etf qdisc refuses a packet whose
 *                                            launch time is nearer than `delta`.
 *                                            300 us is the value proven on the
 *                                            repacer's rig ports; 80 us was
 *                                            ear-validated on one of them and read
 *                                            "slightly beepy" on another
 *                                            (REAC-RIG-PARKED-STATE.md).
 *                                            ------
 *   default                        2500 us   round up over the sum.
 *
 * A lead shorter than the worst tail is a lead the thread will miss; a longer one
 * only adds latency, and a lead is buffered audio, so it has to stay far inside the
 * TX ring's ~250 ms cap.
 *
 * reac_repacer's own default is 4 ms (`--etf-lead-ms`, reac_repacer.c:144). NO
 * MEASUREMENT justifies that number anywhere in that tree — it is a round margin
 * over expected scheduler lateness, and it is the same order as this one. Ours is
 * derived from the soak this pacer actually ran, and it is spelled in
 * MICROSECONDS, because a millisecond knob cannot express the difference the
 * comparative run is being asked to resolve.
 *
 * Knob: REACPW_PACER_LEAD_US, resolved through reac_conf's layers. */
#define REAC_ETF_LEAD_US_DEFAULT  2500u
#define REAC_ETF_LEAD_US_MIN        50u   /* below a typical qdisc delta: refused */
#define REAC_ETF_LEAD_US_MAX     50000u   /* 50 ms of launch-time buffering is already
                                           * more than the TX ring's ~250 ms cap wants */

/* Validate a lead in microseconds. REAC_ETF_REFUSE_BAD_LEAD outside the bounds. */
enum reac_etf_refusal reac_etf_lead_check(unsigned lead_us);

/* ---- the qdisc ------------------------------------------------------------ *
 *
 * THE ONE THAT COST THE PRIOR ART WEEKS. reac_repacer ran with `--etf` for months
 * on a port whose root qdisc was `noqueue`: SO_TXTIME was set, SCM_TXTIME was
 * stamped on every frame, and the kernel ignored all of it — the frames left at
 * sendmsg time with the full thread jitter the option existed to remove, and
 * nothing anywhere said so. It was found by an operator's ear, not by the daemon
 * (REAC-REPACE-MASTER-CLOCK-LIMIT.md, finding 1). A stamp nobody honours is
 * indistinguishable from no backend at all, so this is checked before the backend
 * claims to be running.
 *
 * The probe is an RTM_GETQDISC dump over the rtnetlink socket the transport already
 * speaks, filtered to one ifindex: no `tc` subprocess (the daemon has none, by
 * ruling), no parsing of a command's output. It accepts an etf qdisc ANYWHERE on
 * the device, not only at the root, because on a multiqueue NIC etf is attached per
 * TX queue under an `mq` root — see docs/ETF-PACING.md for both forms. */
enum reac_etf_qdisc {
	REAC_ETF_QDISC_UNREADABLE = 0,  /* the dump failed — NOT the same as absent */
	REAC_ETF_QDISC_ETF,             /* etf is attached: launch times are honoured */
	REAC_ETF_QDISC_ABSENT,          /* the device has qdiscs, none of them etf */
};

/* Probe `ifindex` for an etf qdisc. `kind` (may be NULL) receives the name of the
 * ROOT qdisc found, so a refusal can say what is there instead of what is not.
 *
 * UNREADABLE IS NOT ABSENT. A dump that could not be made says so with its own
 * value: reporting "no etf" from a netlink socket that never opened is the
 * broken-search failure, and it would refuse a correctly configured rig. */
enum reac_etf_qdisc reac_etf_qdisc_probe(int ifindex, char *kind, size_t cap);

/* ---- the socket ----------------------------------------------------------- *
 *
 * The seam a test can drive. `setopt` is setsockopt's shape; the test passes a fake
 * that refuses, so "the backend refuses without SO_TXTIME support" is proven without
 * a kernel that lacks it. Pass NULL for the real setsockopt.
 *
 * `tai_offset_s` is the kernel's TAI-UTC offset in seconds — reac_etf_tai_offset()
 * reads it; a test passes 0 to prove the refusal fires. */
typedef int (*reac_etf_setopt_fn)(int fd, int level, int optname,
                                  const void *val, socklen_t len);

/* The kernel's current TAI-UTC offset in seconds, from adjtimex. 0 means nothing has
 * disciplined this clock and CLOCK_TAI is really UTC. Negative on a failed call. */
int reac_etf_tai_offset(void);

/* Put `fd` into launch-time mode: SO_TXTIME with clockid CLOCK_TAI and
 * SOF_TXTIME_REPORT_ERRORS, so a packet the qdisc drops as too-late comes back on
 * the socket's error queue instead of vanishing.
 *
 * DEADLINE MODE IS OFF, deliberately. In deadline mode the qdisc may send EARLY —
 * the launch time becomes "no later than". A REAC slave recovers its word clock
 * from the inter-arrival interval, so early is exactly as wrong as late; we want
 * strict mode, where the packet leaves AT the time and not before.
 *
 * Returns REAC_ETF_OK, or the refusal code. */
enum reac_etf_refusal reac_etf_socket_arm(int fd, int tai_offset_s,
                                          reac_etf_setopt_fn setopt);

/* Build the SCM_TXTIME control message for one frame into `cmsgbuf` (which must be
 * at least REAC_ETF_CMSG_SPACE bytes) and point `msg` at it. `msg` must already
 * carry its iov and name. Returns the number of control bytes used. */
#define REAC_ETF_CMSG_SPACE (CMSG_SPACE(sizeof(uint64_t)))
size_t reac_etf_stamp(struct msghdr *msg, void *cmsgbuf, uint64_t launch_ns);

/* Drain the socket's error queue and return how many frames the qdisc REFUSED —
 * a launch time already in the past, or one beyond the qdisc's horizon. With
 * SOF_TXTIME_REPORT_ERRORS armed this is the only place those losses are visible;
 * without it a late frame is dropped and nothing anywhere says so, which is the
 * silent-failure shape this whole backend has to avoid. Non-blocking, bounded, and
 * safe to call from the RT slot loop: it makes at most `budget` recvmsg calls.
 *
 * `first_code` receives the SO_EE_CODE of the first refusal seen (0 when none), so
 * the journal can say WHICH refusal rather than only how many. May be NULL. */
unsigned reac_etf_drain_errors(int fd, unsigned budget, uint8_t *first_code);

/* CLOCK_TAI now, in nanoseconds. 0 when the clock is unreadable — the caller checks,
 * because a launch time computed from a zero base would be 1970. */
uint64_t reac_etf_tai_ns(void);

#endif /* REAC_ETF_H */
