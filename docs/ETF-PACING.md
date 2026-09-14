# Launch-time pacing: `SO_TXTIME` + the ETF qdisc

The pacer has two backends. They share one loop, one FSM, one FILLER, one depth guard and one
set of counters; what differs is **who decides the instant a frame leaves the machine**.

| | `REACPW_PACER=thread` (default) | `REACPW_PACER=etf` |
|---|---|---|
| the egress instant is | this thread's wake | a launch time the kernel holds the frame until |
| the thread must be | **punctual** | **early**, by one lead |
| syscall | `sendto` | `sendmsg` + `SCM_TXTIME` |
| slot clock | `CLOCK_MONOTONIC` | `CLOCK_TAI` |
| needs | nothing | an etf qdisc, `SO_TXTIME`, a disciplined TAI offset |

Nothing about the default changes. With the knob unset the handle's backend fields stay zero and
the loop never reads one — the emitted bytes and their timing are what they were.

## Why

A REAC slave recovers its word clock from the master's frame inter-arrival interval, so the
cadence *is* the clock. On the thread backend every scheduling tail between the wake and the
syscall lands on the wire. This pacer's own instrumentation measures that tail: **3.6 late slots/s
and 900 ppm of transmit deficit** on the live rig, worst single slot debt 2000 µs over a 30-minute
soak (`reac_pacer.h`).

`reac_repacer`, the OpenWrt de-jitter relay, is the prior art for the cure and measured it:
switching the same mechanism on tightened a relay's egress cadence from **3.6 µs to 1.4 µs** of
jitter (`reac-aes67-split-src/docs/design/specs/2026-06-11-etf-localin-clock-recovery.md`). Two of
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

All three are checked at `reac_pacer_open`. **If the operator asked for ETF and one of them is
missing, the open FAILS and names the code.** It does not fall back to the thread backend: a run
that believes it is measuring launch-time pacing while the thread is doing the pacing is worse than
no run at all.

| refusal | what it means | fix |
|---|---|---|
| `REAC_ETF_REFUSE_NO_QDISC` | the netdev has no etf qdisc anywhere; a launch time would be stamped on every frame and ignored | the `tc` commands below |
| `REAC_ETF_REFUSE_NO_TXTIME` | `setsockopt(SO_TXTIME)` refused | a kernel ≥ 4.19 with `CONFIG_NET_SCH_ETF` |
| `REAC_ETF_REFUSE_TAI_UNSET` | the kernel's TAI offset reads 0, so `CLOCK_TAI` is really UTC and every launch time would be 37 s from where the qdisc reads its own clock | discipline the clock (`chronyd`/`ntpd` sets the offset; `adjtimex` reports it) |
| `REAC_ETF_REFUSE_NO_TAI_CLOCK` | `adjtimex` itself failed | — |
| `REAC_ETF_REFUSE_BAD_LEAD` | `REACPW_PACER_LEAD_US` outside [50, 50000] | — |

**The qdisc check is the one that matters most.** `reac_repacer` ran with `--etf` for months on a
port whose root qdisc was `noqueue`: `SO_TXTIME` was set, `SCM_TXTIME` was stamped on every frame,
and the kernel ignored all of it. It was found by an operator's ear, not by the daemon
(`REAC-REPACE-MASTER-CLOCK-LIMIT.md`, finding 1). The probe here is an `RTM_GETQDISC` netlink dump
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
    the qdisc's own `delta`         300 µs   proven on the repacer's rig ports
                                             (80 µs was ear-validated on one and read
                                              "slightly beepy" on another)
                                   -------
    REACPW_PACER_LEAD_US default   2500 µs

`reac_repacer`'s own default is 4 ms (`--etf-lead-ms`). **No measurement justifies that number
anywhere in that tree** — it is a round margin over expected scheduler lateness. Ours is derived
from the soak this pacer actually ran, and is spelled in microseconds because a millisecond knob
cannot express the difference the comparative run is being asked to resolve.

A lead is buffered audio: it is latency, and it must stay far inside the TX ring's ~250 ms cap.

## The `tc` commands

`sch_etf` is a module and is **not loaded by default** on the desk:

    sudo modprobe sch_etf

### The desk as it is today (software ETF)

Read off the rig, 2026-09-14:

    enp131s0        root qdisc fq_codel     Realtek RTL8125 2.5GbE, driver r8169
    enp131s0.11     root qdisc noqueue
    enp131s0.12     root qdisc noqueue
    enp131s0.13     root qdisc noqueue

Two things follow, and both bound what this desk can prove:

- **No hardware offload here.** `ethtool -T enp131s0` reports `software-transmit` /
  `software-receive` only and `PTP Hardware Clock: none`. `r8169` has no ETF offload. So `offload`
  will be refused and everything below is **software ETF**: the kernel's hrtimer releases the
  packet, which removes the *thread's* wake jitter but not the driver's. A hardware-launch arm needs
  a different NIC — see the next section.
- **The VLAN devices are `noqueue`**, which is precisely the silent-no-op condition above. The ETF
  backend refuses on them today, by code, until a qdisc is attached.

Per VLAN sub-interface (`delta` is the 300 µs proven on the repacer's ports):

    sudo tc qdisc replace dev enp131s0.11 root etf clockid CLOCK_TAI delta 300000 skip_sock_check
    sudo tc qdisc replace dev enp131s0.12 root etf clockid CLOCK_TAI delta 300000 skip_sock_check
    sudo tc qdisc replace dev enp131s0.13 root etf clockid CLOCK_TAI delta 300000 skip_sock_check

    tc qdisc show dev enp131s0.11        # must print `qdisc etf`, not `noqueue`

To undo, restoring the device to what it was:

    sudo tc qdisc del dev enp131s0.11 root     # back to noqueue

**Why `skip_sock_check`, and what it costs.** Without it `sch_etf` drops every packet from a socket
that did not set `SO_TXTIME` — and the pacer's socket is not the only thing that transmits on a
segment. With it, unstamped packets are not refused *on the socket check*; a packet carrying no
launch time at all can still be dropped once the queue is non-empty, because its `tstamp` of 0 reads
as already expired. **This is not proven on the desk.** It is the first thing the comparative run
has to check, and it is checked by a ratio the box itself produces — the S-4000's upstream frame
rate and its ESTABLISHED state — never by the absence of an error message. `iproute2` 6.17.0 accepts
`skip_sock_check` even though its usage line does not list it.

**A note on the device names.** The 2026-09-12 ruling names VLAN sub-interfaces `reacA`, `reacB`,
`reacC` (`<nic>.<vid>` overflows `IFNAMSIZ` on some NIC names). The desk is **not** on those names
today — it carries `enp131s0.11/.12/.13` — so the commands above are written for the devices as they
actually are. Substitute the minted names wherever the rig has moved to them.

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

To put the desk on the ETF arm for its window:

    sudo modprobe sch_etf
    sudo tc qdisc replace dev enp131s0.11 root etf clockid CLOCK_TAI delta 300000 skip_sock_check
    tc qdisc show dev enp131s0.11                      # confirm `etf`, not `noqueue`
    REACPW_PACER=etf REACPW_PACER_LEAD_US=2500 <the daemon's usual start>

and to put it back on the thread arm:

    REACPW_PACER=thread <the daemon's usual start>
    sudo tc qdisc del dev enp131s0.11 root

The journal names the backend and the layer that chose it on every open, so which arm is running is
read off the daemon rather than remembered.

## What this does not prove

- **Nothing here has run on the rig.** The backend has unit tests and a clean build; no frame has
  left a NIC with a launch time on it.
- **Software ETF only, on this desk.** The RTL8125 has no PTP hardware clock and no ETF offload, so
  a hardware-launch arm is not measurable here at all. Any figure this desk produces is about the
  kernel's hrtimer release, not about a NIC's.
- **`skip_sock_check`'s effect on the segment's other transmitters is unverified**, as above.
- The 2500 µs lead's two terms are each measured, but **the sum has never been swept**; the
  comparative run is where a shorter lead gets tried.
