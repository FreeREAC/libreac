# A kernel-module backend behind `struct reac_handle`

Status: draft for the operator

- **Author:** Pau Aliagas <linuxnow@gmail.com>
- **Governs:** how the REAC realtime data path may run inside a Linux kernel module while every
  public call in `libreac-transport` keeps its present shape.
- **Conforms to, does not reopen:** `2026-09-11-reac-transport-library.md` (the three layers, the
  backend-agnostic API, the opaque OS handle, §7's open reactor seam); `docs/REAC-CONTROL-PLANE.md`;
  openmixer's `2026-08-23-reac-trunk-vlan-daemon.md` and `2026-08-20-reac-master-arbitration.md`.
  None of the behaviour those specify changes here — only where the cadence and the frame copies
  happen.
- **Operator ruling, 2026-09-13:** "the current design allows to create a kernel module
  transparently — this is important; we can even create it, it is no longer complicated, we have
  cleaned up the namespace."

## 0. The design gate

**Which primitive is this an instance of.** A second backend behind `struct reac_handle`. The
transport spec's §3 already made the OS handle an incomplete type: `reac_handle.h` declares
`struct reac_handle;` and nothing else, and the seven transport objects that reach the segment
(`reac_tx`, `reac_pacer`, `reac_slave`, `reac_seglock`, `reac_ifscan`, `reac_linkmon`, the topo tap)
each hold exactly one pointer to it. The userspace backend completes it as one POSIX descriptor in
`transport/src/reac_handle_priv.h`. A kernel backend completes it as a descriptor on `/dev/reacN`
plus the mapped rings. No public struct changes shape, no caller learns a new lifecycle. This is
not a new layer; it is the second implementation of a seam cut for exactly this.

**What vocabulary extends first.** Four words, and no more: *device* (a `/dev/reacN` node standing
for one bound segment), *slot header* (the per-frame metadata a shared ring carries), *slot debt*
(already the pacer's word, now crossing a boundary) and *steer* (one integer period pushed down
from the userspace DLL). `libreac`'s protocol vocabulary is untouched — the module learns no REAC
word at all.

**Why is the kmod not just the userspace pacer with SCHED_FIFO.** Because SCHED_FIFO is a request
to a scheduler and an hrtimer is not. What the kernel buys: *jitter* — the pacer's own
instrumentation measures late wakes on the live rig at 3.6 slots/s, 900 ppm of transmit deficit,
and that deficit is the whole cause of the TX ring's growth and so of the depth guard's discards
(`reac_pacer.h`, slot-debt comment); an hrtimer callback handing a built `skb` to `dev_queue_xmit`
never waits to be scheduled. *No scheduler dependence* — no `mlockall`, no `RLIMIT_MEMLOCK`, no
rtkit grant, no priority band to place beneath the graph, no CPU pin, so a host running a suite or
a browser cannot starve the cadence. *No syscall per frame* — 8000 `sendto()` per second per
segment at 96 kHz become a write into a shared page. *A char device the console opens* — `/dev/reacN`
is a udev object with an owner and a mode, so permission becomes group membership rather than
`%caps` on a binary.

What it costs. *A licence firewall:* the kernel is GPL-2.0-only and "or-later" reaches forward,
never back, so `libreac` (GPL-3.0-or-later) can never be linked into a module — `reac-kmod`'s
`NOTICE` records this and its smoke test already gates "no libreac symbol undefined". The
consequence is architectural: **the module never parses a REAC frame**, and that is what keeps it
small. *Two backends to keep conformant:* every netns test runs twice. *A fault is a kernel fault:*
a daemon restart becomes a reboot. *No floating point in kernel:* the DLL stays in userspace and
only integer nanoseconds cross. *DKMS per kernel and a MOK enrolment per machine* under Secure
Boot — both already wired and measured.

## 1. The split

**`libreac` — unchanged, entirely.** Frame layout, the braid, `reac_downstream_build`, the upstream
decode, `reac_ctrl`/`reac_master`/`reac_hunt`/`reac_fsm`, the head-amp model, `reac.ksy`. No lane
below touches `src/` or `include/reac/*.h`.

**`libreac-transport` userspace — the whole control plane.** Interface and VLAN discovery
(`reac_ifscan`, `reac_topo`, `reac_vlan`, `reac_linkmon`, `reac_segment_ident`), the establishment
machines (`reac_slave`, `reac_pacer`'s `struct reac_master` half, courtship, grant, chanmap,
cold-connect), the head-amp table, the clock discipline and its DLL, layered config, the refusal
enums, and `reac_ring`. All of it is protocol-shaped, all of it needs `libreac`, so all of it stays
on the GPL-3 side of the firewall.

**The module — the RT data path and nothing else.** Emit one pre-built frame per slot from an
hrtimer; copy every accepted 0x8819 frame verbatim into a shared ring; hold the segment claim. It
recognises an EtherType and a netdev; it does not know what a grant is.

**The boundary.** One character device per bound segment. Audio and control frames cross by `mmap`,
never by `ioctl`; configuration and statistics cross by `ioctl`; asynchronous facts cross by
`read()`, with `poll()` separating them — `POLLIN` means the RX ring is non-empty, `POLLPRI` means
an event record waits. **Ten ioctls and three event records: thirteen messages.**

| # | message | fields |
|---|---|---|
| 1 | `REAC_IOC_BIND` | in `char ifname[IFNAMSIZ]`, `__u32 flags` → out `__u32 ifindex`, `__u32 abi` |
| 2 | `REAC_IOC_UNBIND` | — |
| 3 | `REAC_IOC_CLAIM` | — → `0`, or `-EADDRINUSE` when the segment is held |
| 4 | `REAC_IOC_RELEASE` | — |
| 5 | `REAC_IOC_RINGS` | in `__u32 tx_slots, rx_slots, up_slots, slot_sz` → out `__u64 tx_off/tx_len, rx_off/rx_len, up_off/up_len` |
| 6 | `REAC_IOC_PACE` | `__u32 fps`, `__u64 period_ns`, `__u32 catchup_max_slots`, `__u32 run` |
| 7 | `REAC_IOC_STEER` | `__u64 period_ns`, `__u32 seq` |
| 8 | `REAC_IOC_TX_TEMPLATE` | `__u16 len`, `__u8 frame[2048]`, `__u8 dst[6]` |
| 9 | `REAC_IOC_RX_FILTER` | `__u32 stream` (downstream/upstream), `__u8 peer[6]`, `__u32 flags` |
| 10 | `REAC_IOC_STATS` | out `tx_frames, tx_errors, late_wakes, slots_catchup, slots_dropped, slot_debt_max, tx_depth_min/peak, rx_frames, rx_drops, up_frames, up_drops` |
| 11 | `REAC_EV_LATE` | `__u64 at_ns`, `__u32 debt_slots`, `__u32 repaid` |
| 12 | `REAC_EV_CARRIER` | `__u64 at_ns`, `__u32 up` |
| 13 | `REAC_EV_CLAIM_LOST` | `__u64 at_ns`, `__u32 reason` |

`TX_TEMPLATE` is the one frame the module may repeat by itself, on a slot where the ring is empty —
the silent FILLER that keeps a slaved box locked. It is bytes the module copies, not bytes it
composes. `STATS` returns exactly the counters `reac_pacer_health_poll` already reports, so the
journal line above them does not change a word.

## 2. The handle, call for call

The kernel backend's `reac_handle` completes as `{ int fd; void *tx, *rx, *up; }` — the descriptor
and the three mapped regions — and the private inline accessor stays one pointer read on the RT
path. `reac_handle_close` unmaps and closes. Which backend a process runs is resolved once, at
open, from a `reac_conf` key (`backend = userspace | kmod`) under the existing precedence law.

| public call | userspace backend | kmod backend |
|---|---|---|
| `reac_tx_open` / `_close` | `socket(AF_PACKET)`, `SIOCGIFINDEX` / `close` | `open("/dev/reacN")`, `BIND` / `UNBIND`, unmap, `close` |
| `reac_tx_emit` | `sendto` | push one slot into the mapped TX ring (no syscall) |
| `reac_seglock_claim` / `_release` | `bind` on the abstract name / `close` | `CLAIM` — the module binds the identical abstract name from a kernel socket / `RELEASE` |
| `reac_seglock_held` | pointer test | pointer test (identical) |
| `reac_pacer_open` | TX socket + `reac_frame_ring_init` | `open`, `BIND`, `RINGS` |
| `reac_pacer_start` | `pthread_create` + `reac_rt_thread_go` | `PACE{run=1}`; no thread exists |
| `reac_pacer_submit` | `reac_frame_ring_push` | the same push, into the shared page |
| `reac_pacer_apply_rate` / `_rate_drain` | recompute on the pacer thread | recompute in userspace, then `PACE{fps, period_ns}` |
| `reac_pacer_clock_publish` / `_publish_graph` | relaxed atomics into `p->clock*` | unchanged — publishers never crossed the boundary |
| `reac_pacer_clock_tick` | returns the steered period to the pacer loop | returns it to the caller, who issues `STEER` |
| `reac_pacer_health_poll` | reads the pacer's atomics | `STATS`, same fields |
| `reac_pacer_pace_source` | mirror read | unchanged |
| `reac_rx_start` | feeder thread + `reac_capture` | feeder thread + `RX_FILTER`, reading the mapped RX ring |
| `reac_ring_*` | unchanged | unchanged — the decoded planar ring is userspace on both arms |
| `reac_slave_*` | FSM + `sendto` | FSM unchanged; its emit becomes a TX-ring push |
| `reac_ifscan_*`, `reac_linkmon_*`, `reac_topo_*`, `reac_vlan_*` | rtnetlink | unchanged — control plane, both arms |
| `reac_ifscan_fd`, `reac_linkmon_fd`, `reac_topo_tap_fd` | the netlink / tap descriptor | unchanged; `/dev/reacN` is a fourth pollable descriptor of the same kind |

That last row makes the transport spec's §7 reactor seam more urgent, not less: `main.c` registers
`reac_ifscan_fd` and `reac_topo_tap_fd` with `pw_loop_add_io` today, and the device descriptor joins
them — unchanged in shape, still open.

## 3. The pacer in the kernel

One `hrtimer` per bound device, `HRTIMER_MODE_ABS_HARD`, advanced with `hrtimer_forward` — the
analogue of the absolute deadline `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` advances by
`period_ns` today, and for the same reason: no drift accumulation.

Period per pace, from `reac_pacer_period_ns(fps)`: 8000 fps → 125 000 ns at 96 kHz, 4000 fps →
250 000 ns at 48 kHz, 3675 fps → 272 108 ns at 44.1 kHz. The third is not an integer — the true
period is 272 108.84 ns, a standing 3.1 ppm of truncation. The userspace pacer inherits the same
truncation and the DLL absorbs it when following is on; a free-running kernel pacer at 44.1 kHz has
no such cover. Where the fractional accumulator lives is open (§9).

The pace code in `cfea[19]` — 0 at 48 kHz, 1 at 96 kHz, 2 at 44.1 kHz — is stamped by userspace
into the frame before it is pushed; the module carries `fps` only to size its period and its debt
budget.

**Clock discipline.** Nothing about how the graph's rate is measured changes. PipeWire's driver
clock reaches `reac_pacer_clock_publish_graph` from whichever of `reac-pw`'s nodes the graph drives
— the 2026-09-08 fix that moved admission out of the sink's callback stands — and the same DLL
ranks PHC > hardware graph clock > box counter slope > free-run, in userspace, in floating point.
Its output is one integer: the period this slot should advance by. That crosses as `STEER`, at the
discipline's own re-evaluation cadence (`REAC_CLOCK_TICK_SLOTS`), not per slot — dozens of ioctls a
second, not eight thousand. `seq` lets the module ignore a reordered steer. With following disabled
no `STEER` is issued and the module runs the `PACE` constant, which is today's inert behaviour.

**Late wakes.** An hrtimer can still be late — a long IRQ-off region, a stalled qdisc — so the law
is carried over rather than assumed away: stay on the grid and repay the debt up to
`catchup_max_slots`; beyond it declare, re-base and count. The module keeps `late_wakes`,
`slots_catchup`, `slots_dropped` and `slot_debt_max` with their present meanings and emits
`REAC_EV_LATE` when the budget is exceeded. These should go to approximately zero; keeping them is
what makes "approximately zero" a measurement instead of a claim.

## 4. The audio path

Three mapped regions per device, fixed-size slot rings with a 16-byte slot header (`len`, `ts_ns`,
`flags`, `vid`) and SPSC atomics in a control page — the discipline `reac_frame_ring` and
`reac_ring` already use.

**TX ring** (userspace producer → module consumer), slot 2048 B to match `REAC_PACER_SLOT_SZ`,
2048 slots ≈ 250 ms ≈ 4 MiB per segment at 96 kHz. `reac_sink_node`'s `process()` de-stages each
quantum into 12-sample frames, encodes with `reac_downstream_build` and writes **directly into the
slot** — the copy `reac_frame_ring_push` performs today, now landing in the shared page; the
`sendto` copy behind it is gone. The depth guard (`HIGH = max(FLOOR, 4 × quantum/12)`,
`TARGET = HIGH/2`) moves into the module, because the consumer owns `tail` and the module is now the
consumer; its constants and telemetry are unchanged.

**RX downstream ring** (module producer → userspace consumer), slot 1536 B, 2048 slots. The module
copies each accepted frame verbatim: 1492 B carrying 1440 B of braided audio, the 40 channels ×
12 samples × 24 bits the frame geometry fixes. The feeder un-braids from the shared page with
`libreac`'s core into `reac_ring` — the same single decode copy it does today, from a mapped page
instead of a socket buffer.

**RX upstream rings**, one per served box MAC, selected by `RX_FILTER`. Separate rings, not a flag
on the first, because `reac_rx.h`'s rule stands: mixing the master's 40-ch broadcast with a box's
`52 + nch×36` return into one ring interleaves two audio sources and corrupts the counter and ppm
tracking the rate authority depends on.

**Say the copy count honestly.** This is not zero-copy. TX loses one copy. RX keeps the count it
has — driver fills an `skb`, module copies the payload into the ring, userspace decodes out of it —
with the `recvfrom` syscall removed. Flipping an `skb` page into userspace is a different design and
is not proposed here.

## 5. Head-amp and control records

They stay in userspace, whole: the head-amp table (`reac_headamp_tx`), the master's per-slot
decision (`reac_master_next`: FILLER / probe / grant / chanmap / cfea announce), the grant burst,
the chanmap sweep, the cold-connect flood and the ~1/s heartbeat are all `libreac` calls and all
GPL-3. A control frame reaches the wire as an audio frame does: userspace builds and stamps the
complete 1492-B frame, sets `REAC_SLOT_CTRL` and the destination MAC in the slot header, and pushes
it. The module emits it on the next slot ahead of queued audio and counts it separately; it reads no
byte of it. The 0.5.6 destination rule survives because it lives in the frame — the address is
stamped into the bytes, not only into the `sendto` — and the slot header's `dst` is what the module
puts in the `skb`'s hardware header.

## 6. VLAN and trunk

The module binds **per netdev**, and a VLAN sub-interface is a netdev. Under the 2026-09-12 ruling
those are named `reacA`, `reacB`, `reacC`… — never `<nic>.<vid>`, which overflows `IFNAMSIZ` on the
rig's own NIC names — so `BIND` takes `reacA` and the kernel has stripped the tag before the packet
handler sees the frame. That is the measured behaviour `reac_topo.h` records: a handler on the
sub-interface sees an untagged frame, and one on the parent sees every VLAN's frames untagged and
indistinguishable. **The parent is therefore never bound** — `BIND` refuses a netdev `reac_topo` has
classified as a trunk, the same refusal the userspace hearing path makes and for the same reason:
binding the parent would put a master on the parent while masters run on its children.

Discovery, the ETH_P_ALL tap, the `PACKET_AUXDATA` VID read, minting and the `reac-pw:minted` alias
stay in userspace. The module learns of a VLAN by being handed a netdev name.

## 7. Capabilities and packaging

`reac-kmod` already ships as a DKMS source RPM (noarch, `AUTOINSTALL=yes`, so a kernel update
rebuilds rather than orphans), with an akmods framework drop-in beside it, signed with the machine's
enrolled MOK. That does not change. Added:

- **udev.** `/dev/reac*` → `GROUP="reac", MODE="0660"`, the group created by the package and
  `reac-pw`'s unit given `SupplementaryGroups=reac`.
- **What userspace stops needing.** `CAP_NET_RAW` for the per-segment audio sockets, TX and RX both;
  `RLIMIT_MEMLOCK` and rtkit for a pacer thread that no longer exists.
- **What it still needs**, so the `%caps` line is not trimmed too far: `CAP_NET_RAW` for the
  ETH_P_ALL topology tap and `reac_ifscan`'s discovery sockets, neither of which moves;
  `CAP_NET_ADMIN` to mint and mark VLAN sub-interfaces. The honest claim is that the kmod backend
  retires the capability *from the audio path*, not from the process.

## 8. Testing

The tiers that exist: offline pcap-replay and pure unit tests in both repositories
(`test_reac_pacer*.c`, `test_reac_slave.c`, `test_reac_courtship.c`, and the `reac_s4000_golden.inc`
/ `reac_m200_golden.inc` / `reac_grant_golden.inc` corpora); real-socket netns and whole-binary
scripts in `reac-pw` (`hearing-finds-a-segment.sh`, `box-master-slave-join.sh`,
`link-up-reestablishes.sh`, `iface-vanish-exits.sh`); the module's `tools/smoke.sh` gates and
`tests/lock-contract.sh`; and the rig.

What the module adds. **`--backend kmod`** (and `REACPW_BACKEND`), so every netns script runs twice
and asserts the same thing on both arms — a userspace-green suite is evidence about one backend
only. **A VM loopback test**, because the module cannot load in a container: a VM on the same kernel
release, a veth or dummy pair, bind one end, drive the cadence, count at the other with the observer
path the module already has, as a ratio over a long window rather than an absolute. **Two inverted
gates:** `smoke.sh` asserts today that no transmit path is linked, which inverts when TX lands and
must be re-sabotaged in its new direction; and that no floating point appears in the disassembly,
which must *stay* — it is now the proof that the DLL did not leak into the kernel. **The licence
gate stays load-bearing:** "no libreac symbol undefined" is the firewall, never relaxed for
convenience. And **every cadence proof is a ratio** — a pps figure taken while the wire is silent is
not evidence, so the baseline must be shown to be a signal before any delta is believed.

## 9. Proven, owed, and the lanes

| fact | state |
|---|---|
| out-of-tree build, `W=1` silent, DKMS lifecycle, noarch RPM | **proven** — built and installed on the rig |
| `dev_add_pack` on 0x8819, per-(netns, ifindex) segment counter | **proven** — 4000.4 frames/s measured live |
| the abstract-namespace segment lock shared with the daemon, both parties refusing | **proven** — two-sided, measured live |
| sysfs door; GPL-2 tag; the no-libreac-symbol and no-floating-point gates | **proven**, sabotage-verified |
| `struct reac_handle` opaque in all seven transport objects | **proven** — libreac-transport 1.0.1, abi 3 |
| hrtimer cadence, `hrtimer_forward`, debt accounting in kernel | owed |
| the three mapped rings and their slot headers | owed |
| the thirteen-message boundary as a UAPI header | owed |
| `STEER`, and the DLL feeding it across the boundary | owed |
| backend selection in `reac_conf`; the kmod `reac_handle` completion | owed |
| the master's slot-anchored stamping under a deep TX ring | owed, **unresolved** |
| the 44.1 kHz fractional period | owed, **unresolved** |
| `BIND` refusing a trunk parent | owed |
| udev rule, `reac` group, the trimmed `%caps` line | owed |
| `--backend kmod` across the netns suite; the VM pair test | owed |
| the §7 reactor seam of the transport spec | owed, and unchanged by this spec |

**The two unresolved rows, named rather than buried.** First, the master's per-slot stamping is
anchored to the slot index — probe stride `fps/500`, a 341-probe burst, `grant_dwell`, one grant per
12 slots over ~150 ms — and a deep TX ring decouples the decision from the slot it lands on by the
ring's depth. Two candidate cures, neither chosen here: run the FSM one ring-depth ahead and carry
the intended slot index in the slot header, so the module can drop a stale control slot; or keep the
ring shallow (≤ 4 slots, 0.5 ms at 96 kHz) until ESTABLISHED and deepen it after. Second, the
44.1 kHz period's 0.84 ns remainder has to accumulate somewhere, in integers, inside a timer whose
own slack and coalescing may swallow it.

**The lanes, in order, each with the proof that ends it.**

1. **TX-only cadence, driving the S-4000 at 96 kHz.** The module gains `BIND`, `RINGS`, `PACE`,
   `TX_TEMPLATE`, `STATS`, the TX ring and the hrtimer. RX, the claim and the steer stay on the
   userspace paths, so the master FSM, the grant burst and the head-amp table are untouched code
   running unchanged. *Proof:* on the rig, `reac-pw --backend kmod` masters the segment; the S-4000
   reaches ESTABLISHED and its upstream counter slope reads 8000 fps as a ratio against
   `CLOCK_MONOTONIC` over ≥ 60 s; a tone injected at the console is present at the box's output well
   above the floor and follows a fader move, so the injection is proven to land where it is believed
   to; `STATS`'s `late_wakes` and `slots_dropped` are compared with the userspace arm's over the
   same window. The negative control runs too — with `PACE{run=0}` the box must drop, or the
   measurement proves nothing about the module.
2. **RX ring** — `RX_FILTER`, the downstream ring, the feeder reading a mapped page. *Proof:* the
   offline decode assertions taken from the mapped ring, and on the rig identical recovered rate and
   ppm slope from both backends over the same minute.
3. **The claim** — `CLAIM`/`RELEASE` wired to the lock the module already holds. *Proof:*
   `lock-contract.sh` refusing in both directions across backends, `/proc/net/unix` showing one
   holder.
4. **Clock steer.** *Proof:* the wire's pps follows a deliberate graph-clock offset as a ratio, and
   with following off the period is bit-identical to the `PACE` constant.
5. **Upstream rings.** *Proof:* a box's microphone audible on its upstream node under both backends.
6. **VLAN.** *Proof:* both of the rig's VLAN segments driven from the module, and a `BIND` on the
   trunk parent refused by name.
7. **Packaging and capabilities.** *Proof:* the daemon drives a box with no `CAP_NET_RAW` on its
   audio path, and removing the group membership refuses cleanly by name.
8. **Conformance** — `--backend kmod` across the netns suite plus the VM pair test in CI. *Proof:*
   both arms green on the same assertions, one sabotage per arm going red.

Lane 1 decides whether the rest is worth building. If the cadence does not measure better than the
userspace pacer on the same rig, the answer is that the SCHED_FIFO thread was enough and this spec
closes.
