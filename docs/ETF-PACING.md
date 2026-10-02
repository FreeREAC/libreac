# Launch-time pacing: `SO_TXTIME` + the ETF qdisc

**This is the default.** `REACPW_PACER=thread` opts out.

The pacer has two backends. They share one loop, one FSM, one FILLER, one depth guard and one
set of counters; what differs is **who decides the instant a frame leaves the machine**.

| | `etf` (the default) | `REACPW_PACER=thread` |
|---|---|---|
| the egress instant is | a launch time the kernel holds the frame until | this thread's wake |
| the thread must be | **early**, by one lead | **punctual** |
| syscall | `sendmsg` + `SCM_TXTIME` | `sendto` |
| slot clock | `CLOCK_TAI` | `CLOCK_MONOTONIC` |
| needs | an etf qdisc, `SO_TXTIME`, a disciplined TAI offset | nothing |

**The daemon installs the qdisc; nobody types a `tc` line on a rig.** The backend and the
qdisc are one setting, and the app that owns a setting owns its configuration. reac-pw puts
`etf clockid CLOCK_TAI delta 300000 skip_sock_check` on the device it binds when the backend
is `etf`, removes a **leftover** etf root when the backend is `thread`, and takes away what it
installed on a clean exit. It does that over rtnetlink from C — no `tc` subprocess. The manual
commands further down are the debugging path, not the operating procedure.

**What happens when the machine cannot run ETF depends on who asked.** With `REACPW_PACER=etf`
set at any layer, an unmet precondition FAILS the open and names the code: a run that believes
it is measuring launch-time pacing while the thread paces is worse than no run. With the knob
unset — the default — the daemon logs one loud line naming the refusal and its fix, runs the
thread backend, and **publishes the refusal** on its own node (`reac.pace.backend` and
`reac.pace.backend-refusal`), because a fallback the operator cannot see is the same silent
no-op wearing a default's clothes.

## Why

A REAC slave recovers its word clock from the master's frame inter-arrival interval, so the
cadence *is* the clock. On the thread backend every scheduling tail between the wake and the
syscall lands on the wire. This pacer's own instrumentation measures that tail: **3.6 late slots/s
and 900 ppm of transmit deficit** on a live segment, worst single slot debt 2000 µs over a 30-minute
soak (`reac_pacer.h`).

`reac_repacer`, the OpenWrt de-jitter relay, is the prior art for the cure and measured it:
switching the same mechanism on tightened a relay's egress cadence from **3.6 µs to 1.4 µs** of
jitter. Two of
its laws are carried here rather than rediscovered:

- **The grid is accumulated, never re-based on `now`.** Substituting `deadline = now + period`
  put that rig **526.7 ppm** off the master where accumulating held it to **8.7 ppm**
  (`reac-repacer/docs/internals.md`). This pacer already had that law; the ETF backend keeps it.
- **A slot is always filled.** The repacer conceals an underrun by repeating the last frame under
  the next counter; this pacer emits a silent FILLER. Same rule, same reason.

Three things the repacer did **not** do are done here, each because its absence cost that project
real time — see *Preconditions*.

## The launch grid is exact

A launch time is absolute, so a rounded period is not absorbed by the next slot the way a relative
sleep absorbs it: it accumulates.

| pace | true period | `reac_pacer_period_ns` | rounding error |
|---|---|---|---|
| 8000 fps (96 kHz) | 125 000 ns | 125 000 | none |
| 4000 fps (48 kHz) | 250 000 ns | 250 000 | none |
| 3675 fps (44.1 kHz) | 272 108.8435… ns | 272 109 | **+0.157 ns/slot** |

That 0.157 ns is 575 ns/s, 34.5 µs/min, 2.07 ms/h — **a whole slot period of phase after about
eight minutes**. So the ETF grid advances by an integer quotient plus an integer remainder
accumulator and never truncates: `launch(n) = base + floor(n · 10⁹ / fps)`, exactly, for every `n`.
`tests/test_reac_etf.c` asserts that at all three paces over 10 s of slots, with the rounded-period
grid as its control (it re-measures the 5 750 ns drift every run, so a green there is never the only
thing asserting it).

## Preconditions, and the refusal for each

All three are checked at `reac_pacer_open`, and what happens next turns on **who asked**.
`REACPW_PACER=etf` at any layer is the operator asking: the open FAILS and names the code,
because a run that believes it is measuring launch-time pacing while the thread is doing the
pacing is worse than no run at all. The knob unset is the *default* asking: the daemon logs
the refusal and its fix, runs the thread backend, and publishes `reac.pace.backend=thread`
with `reac.pace.backend-refusal` naming the reason.

| refusal | what it means | fix |
|---|---|---|
| `REAC_ETF_REFUSE_NO_QDISC` | the netdev has no etf qdisc anywhere; a launch time would be stamped on every frame and ignored | the daemon installs it — see the previous section; a refusal here means the install was refused, and that line names the errno |
| `REAC_ETF_REFUSE_NO_TXTIME` | `setsockopt(SO_TXTIME)` refused, and not for want of a capability | a kernel ≥ 4.19 with `CONFIG_NET_SCH_ETF` |
| `REAC_ETF_REFUSE_TXTIME_EPERM` | `SO_TXTIME` exists and this process may not set it | `CAP_NET_ADMIN` — see below |
| `REAC_ETF_REFUSE_TAI_UNSET` | the kernel's TAI offset reads 0, so `CLOCK_TAI` is really UTC and every launch time would be 37 s from where the qdisc reads its own clock | discipline the clock (`chronyd`/`ntpd` sets the offset; `adjtimex` reports it) |
| `REAC_ETF_REFUSE_NO_TAI_CLOCK` | `adjtimex` itself failed | — |
| `REAC_ETF_REFUSE_BAD_LEAD` | `REACPW_PACER_LEAD_US` outside [50, 50000] | — |

**`SO_TXTIME` is capability-gated, and that is a separate code on purpose.** Measured on kernel
7.1.9, uid 0 inside a container holding `NET_RAW` but not `NET_ADMIN`: `EPERM` on both an
AF_PACKET and a UDP socket. It is the *option* that needs `CAP_NET_ADMIN`, not the qdisc — so the
refusal is `REAC_ETF_REFUSE_TXTIME_EPERM`, never "this kernel has no SO_TXTIME", because the two have
different fixes and folding them together sends an operator after a kernel upgrade to cure a
capability. **reac-pw holds `CAP_NET_ADMIN`** — granted by file capability on `%{_bindir}/reac-pw` in the
RPM, for the VLAN sub-interfaces it mints and now for `SO_TXTIME` and the qdisc install. A
probe or a daemon run by hand from a shell has neither and will be refused with `EPERM`.

**The qdisc check is the one that matters most.** `reac_repacer` ran with `--etf` for months on a
port whose root qdisc was `noqueue`: `SO_TXTIME` was set, `SCM_TXTIME` was stamped on every frame,
and the kernel ignored all of it. It was found by ear, not by the daemon. The probe here is an `RTM_GETQDISC` netlink dump
filtered to one ifindex — no `tc` subprocess — and it distinguishes **unreadable** from **absent**:
an unreadable dump warns and arms anyway, because a probe that fails closed would refuse correctly
configured rigs.

`SOF_TXTIME_REPORT_ERRORS` is armed and the socket's error queue drained eight times a second, so a
frame the qdisc refuses is counted into `tx_errors` instead of vanishing. The repacer left that off
too (`flags = 0`).

**Strict mode, not deadline mode.** `SOF_TXTIME_DEADLINE_MODE` lets the qdisc release *early* — the
launch time becomes "no later than". A slave recovering its clock from the interval finds early
exactly as wrong as late, so it is deliberately not set.

## The lead

How far ahead of a frame's launch time the thread hands it down. It is applied to the **wake**, not
to the stamp: the thread sleeps to `launch − lead`, submits, and the kernel owns the instant.

    the thread's worst wake tail   2000 µs   MEASURED, this pacer's 30-minute soak
                                             (p50 250, p90 500, p95 750, worst 2000)
    the qdisc's own `delta`         300 µs   proven on the repacer's ports
                                             (80 µs held on one port and was audibly
                                              marginal on another)
                                   -------
    REACPW_PACER_LEAD_US default   2500 µs

`reac_repacer`'s own default is 4 ms (`--etf-lead-ms`). **No measurement justifies that number
anywhere in that tree** — it is a round margin over expected scheduler lateness. Ours is derived
from the soak this pacer actually ran, and is spelled in microseconds because a millisecond knob
cannot express the difference the comparative run is being asked to resolve.

A lead is buffered audio: it is latency, and it must stay far inside the TX ring's ~250 ms cap.

## What the daemon does for you

On every start, for the device it binds:

| backend | what reac-pw does to the device's root qdisc |
|---|---|
| `etf` (default) | del, then add `etf clockid CLOCK_TAI delta 300000 skip_sock_check` |
| `thread` | remove a **leftover** `etf` root, if one is there; leave anything else alone |

and on a clean exit it removes exactly what it installed, verified by a netlink read-back
before the delete is issued. The two hazards this closes:

- **The qdisc and the backend are one setting.** With `skip_sock_check`, the etf qdisc drops
  every frame that carries no launch time. A daemon running the thread backend under a
  leftover etf qdisc therefore transmits *nothing*: 0 frames in a 60 s window, and the box
  loses its master.
- **`tc qdisc replace` fails on an existing etf qdisc** ("Change operation not supported by
  specified qdisc"): the discipline supports no change operation, so the install is `del`
  then `add`.

It needs `CAP_NET_ADMIN`, which the reac-pw RPM grants by file capability. Started by hand
from a shell the daemon has neither that nor `SO_TXTIME`, and the ETF default falls back to
the thread backend saying `EPERM`.

`sch_etf` is a module. The kernel autoloads it when the qdisc is requested; a kernel with no
`sch_etf` at all answers `ENOENT`, and the loud line says `modprobe sch_etf`.

## The `tc` commands — the debugging path

Nothing below is part of normal operation. It is how to look at, or stand in for, what the
daemon did. `<dev>` is the device the daemon binds (a VLAN sub-interface on a trunk, or the
physical NIC on a direct link).

### A NIC with no ETF offload (software ETF)

    tc qdisc show dev <dev>              # a VLAN sub-interface is `noqueue` by default
    ethtool -T <nic>                     # `software-transmit` only, `PTP Hardware Clock: none`

Most onboard NICs (Realtek `r8169`, USB adapters) report only software timestamping and have no
ETF offload. There `offload` is refused and everything below is **software ETF**: the kernel's
hrtimer releases the packet, which removes the *thread's* wake jitter but not the driver's. A
hardware-launch arm needs a different NIC — see the next section. A `noqueue` device is
precisely the silent-no-op condition above: the ETF backend refuses on it, by code, until a
qdisc is attached.

Per device (`delta` is the 300 µs proven on the repacer's ports):

    sudo tc qdisc replace dev <dev> root etf clockid CLOCK_TAI delta 300000 skip_sock_check
    tc qdisc show dev <dev>              # must print `qdisc etf`, not `noqueue`

To undo, restoring the device to what it was:

    sudo tc qdisc del dev <dev> root     # back to noqueue

**Why `skip_sock_check`, and what it costs.** Without it `sch_etf` drops every packet from a socket
that did not set `SO_TXTIME` — and the pacer's socket is not the only thing that transmits on a
segment. With it, unstamped packets are not refused *on the socket check*; a packet carrying no
launch time at all can still be dropped once the queue is non-empty, because its `tstamp` of 0 reads
as already expired. **This is not yet proven on hardware.** It is the first thing the comparative
run has to check, and it is checked by a ratio the box itself produces — the S-4000's upstream frame
rate and its ESTABLISHED state — never by the absence of an error message. `iproute2` 6.17.0 accepts
`skip_sock_check` even though its usage line does not list it.

### A hardware-capable NIC (hardware launch)

Intel **i210 / i225 / i226** (`igc`/`igb`) have ETF hardware offload on their TX queues and a PTP
hardware clock. There `etf` is attached per TX queue under an `mq` root, and `offload` moves the
launch into the NIC:

    sudo tc qdisc replace dev <nic> root handle 100 mq
    sudo tc qdisc replace dev <nic> parent 100:1 etf clockid CLOCK_TAI delta 300000 offload
    tc qdisc show dev <nic> | grep etf

The backend's probe accepts an etf qdisc found **anywhere** on the device, not only at the root, so
this form passes the precondition unchanged.

Explicitly **not** capable, from the repacer's own survey: MediaTek mt7531/mt7986/mt7988 (hardware
offload refused with `Error: Specified device failed to setup ETF hardware offload`, and no PHC).

## The comparative run

`tools/pace-compare.sh` takes all three arms with one instrument over one window:

    make pace_hist
    tools/pace-compare.sh --iface <mirror> --fps 8000 --secs 60 \
        --arms thread,etf,kmod --out /var/tmp/pace-$(date +%F)

It never restarts the daemon and never switches the backend — on a live console that is the
operator's action. It pauses and asks for each arm to be put in place.

To put a machine on the ETF arm for its window:

    sudo modprobe sch_etf
    sudo tc qdisc replace dev <dev> root etf clockid CLOCK_TAI delta 300000 skip_sock_check
    tc qdisc show dev <dev>                            # confirm `etf`, not `noqueue`
    REACPW_PACER=etf REACPW_PACER_LEAD_US=2500 <the daemon's usual start>

and to put it back on the thread arm:

    REACPW_PACER=thread <the daemon's usual start>
    sudo tc qdisc del dev <dev> root

The journal names the backend and the layer that chose it on every open, so which arm is running is
read off the daemon rather than remembered.

### Measured on the transmitting device

A capture on the *transmitting* device (a mirror port hides most of the difference). Same build
for both arms, one S-4000S-3208 per link, 60 s at 8000 fps, `pace_hist` at 1 µs:

| arm | interval sd µs | p99 µs | p99.9 µs | late ≥ 1.5× /s | late ≥ 4× /s | catch-up /s |
|---|---|---|---|---|---|---|
| PCI VLAN, thread | 28.5 | 134 | 595 | 37.0 | 13.4 | 96.3 |
| PCI VLAN, etf | 2.7 | 127 | 136 | 0.45 | 0.03 | 0.55 |
| USB direct, thread | 15.3 | 131 | 308 | 27.0 | 2.3 | 41.3 |
| USB direct, etf | 1.9 | 128 | 131 | 0.44 | 0.00 | 0.49 |

Both NICs are software-only (no PHC), so the etf rows measure the kernel's hrtimer release, not a
hardware launch. The daemon's own CPU did not rise (1275 vs 1612 process jiffies on the PCI pair).

## Hardware launch: not implemented

Everything measured above is **software ETF**. On an i225/i226 (`igc`) the etf qdisc would sit on
a hardware TX queue of the physical NIC under an `mqprio` parent with `offload`, with the NIC's
PHC disciplined to `CLOCK_TAI`; a VLAN sub-interface has no TX queue of its own to offload onto.
The daemon does not install that form today, and the `delta` and lead above are derived for the
software path only.

## What this does not prove

- **The daemon-installed qdisc is proven on a veth in a private namespace**
  (`tests/etf-qdisc-owned.sh` in reac-pw) and by the byte-exact builder test; the pacing arms in
  the table above were measured with the qdisc put there by hand.
- **Software ETF only.** The NICs measured have no PTP hardware clock and no ETF offload, so every
  figure here is about the kernel's hrtimer release, not about a NIC's.
- **`skip_sock_check`'s effect on the segment's other transmitters is unverified**, as above.
- **No veth arm of `tools/etf-veth-probe.sh` has run.** It submits a burst with launch times and
  requires it to ARRIVE as a grid, with an unstamped burst as its control; it needs iproute2 and
  `CAP_NET_ADMIN`.
- The 2500 µs lead's two terms are each measured, but **the sum has never been swept**; the
  comparative run is where a shorter lead gets tried.
