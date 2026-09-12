# An ungranted courtship is BOUNDED: court, then get off the wire

Status: measured and ruled 2026-09-12; implemented in `src/reac_fsm.c` this lane. Supersedes
`include/reac/reac_fsm.h`'s standing sentence "until the master's grant lands (no hard give-up
while PHY stays up)" — that sentence described `FSM_COLDCONNECT` correctly and was wrong about
what it costs a segment.

- **Author:** Pau Aliagas <linuxnow@gmail.com>
- **Evidence:** `reac-captures` 85c1e97,
  `m200-master-441k-2026-09-11/box-boot-with-slave-analysis.md` — the failure capture
  (`box-boot-with-our-slave-present-12h03-12h06.pcap`, 1 099 400 REAC frames, VLAN 12) and its
  control (`m200-441k-headamp-ch16.pcap`, the same desk and box with our slave absent).

## 1. The measured mechanism

An M-200 keeps exactly **one** box session per segment, and its liveness test is fed by any
upstream stream of the enrolled geometry, **regardless of source MAC**. An ungranted reac-pw
slave streams exactly that geometry at wire rate, forever, so:

| | slave present (failure) | slave absent (control) |
|---|---|---|
| desk's `cfea` after the box goes quiet | width `0x10`, enrolled `0x0001`, unchanged for 80.7 s (84 announces) | flips to `0x08` / `0x0000` at **+7.148 s** |
| desk scene transfers in that window | **0** | 4 116, first at +7.156 s, repeating ~2.7 s |
| box re-enrols | only after the desk was rebooted AND we left | 10 s after replug |

So the desk's **session hold is 7.148 s**, and a box booting beside an ungranted slave has
nothing to join because the desk never declares the slot free.

## 2. The law

A slave that has courted without ever being granted **stops transmitting for longer than the
master's session hold** before it courts again. Two constants, named once in
`include/reac/reac_fsm.h`, both **seconds** (they are wall-clock facts about the master) and
scaled to frames by the FSM's own `heartbeat_period`, which IS fps (`sample_rate / 12`):

- `REAC_FSM_COLDCONNECT_BUDGET_S 4` — how long `FSM_COLDCONNECT` may court ungranted. A real box
  is granted ~1.7 s after its presence flood ends, so 4 s is ~2x generous.
- `REAC_FSM_BACKOFF_S 10` — the new `FSM_BACKOFF` state emits **nothing** for this long, then
  re-floods. 10 s > 7.148 s, by 2.85 s, on every cycle.

A **granted** courtship never reaches the budget: the check is guarded on `grant_ack == 0`, and a
grant opens that window in the same step, before the check runs.

## 3. What this does NOT claim

- The 10 s wall-clock bound holds while the FSM is stepped at ~fps, which is what a flooding
  master does (the M-200 sends 3 673 filler/s). Against a **silent** master the transport
  self-clocks off a 5 ms RX timeout, so the same frame count spans longer — that is safe (more
  silence, not less) but it is not 10 s.
- Whether the desk's liveness keys on the **geometry** or merely on frames arriving is not
  settled by these captures; both readings predict every byte. It matters only for a
  second-order fix (announcing a distinct width), which is **guessed, not proven**, and is not
  in this spec.
- **The desk-side effect is unproven offline.** That a real M-200 releases the slot and courts a
  booting S-1608 during our 10 s silence follows from the control capture, but it has not been
  observed with the patched slave on a wire. It needs the rig.

## 4. How it is held

- `tests/test_link.c` — the pure FSM driven by a master that announces and never grants for 25 s
  of steps at 3 675 fps: COLDCONNECT ends within the budget, the following silence is a full
  `REAC_FSM_BACKOFF_S` and longer than 7.148 s, and it ends in a fresh `FSM_ACT_FLOOD_BCAST`.
  Red on the unpatched FSM at `cold_left > cold_entered`.
- `reac-pw` `tests/courtship-backs-off.sh` + `tests/courtship_probe.c` — the real
  `reac_slave_open`/`reac_slave_start` on an AF_PACKET socket over a veth pair, against a
  master that broadcasts a desk-shaped downstream at 3 675 fps with a `cfea` announce 1/s and
  never grants. The slave's frames are timestamped at the FAR END, which is the only place
  "we are off the wire" is a measurement rather than a self-report. Measured on the desk over
  a 40 s run, master pacing 3 675.0 fps:

  | | frames heard | first burst | longest silence | bursts |
  |---|---|---|---|---|
  | patched | 60 480 | **5.485 s** (1.486 flood + 4.000 budget) | **10.000 s** | 3 |
  | unpatched | 139 773 | never ends | 1.459 s (the run's own tail) | 1 |

  It skips (77) where the namespaces are unavailable, and where the master could not hold its
  pacing to within 15 % of the target — the bounds are frame periods, so a wire whose rate
  moved cannot answer a wall-clock question, and a measurement that could not be taken is not
  a pass.


## Rig result 2026-09-12 (M-200 master at 44.1 kHz, S-1608, reac-pw 1.0.1 on the same VLAN)

The change does what §2 says and not what §1 hoped. Live beside the desk the slave cycles
COLDCONNECT → BACKOFF → FLOOD_ANNOUNCE exactly as measured offline (duty 100 % → 37 %). But the
box-reboot case is NOT solved, twice over:

- Trial 1 — our 16-input slave GRANTED beside the enrolled S-1608; the box rebooted; mute for
  180 s. The courtship never engaged: a granted slave streams, and its stream of the box's
  geometry keeps the desk's one box session alive.
- Trial 2 — our slave announcing 8 inputs, refused by the desk while its box was present and
  backing off as designed; the box rebooted; within the box's silence the desk GRANTED our
  courting slave, and the returning box was blocked as in trial 1. A second power-cycle changed
  nothing.
- Control, both times — our slave off the segment: the box enrolled 9–12 s later.

So a desk grants whichever slave courts while its box is away, and any granted slave of that
geometry then blocks the box. Bounding the courtship reduces wire load and is kept; it is not a
fix. The product rule stands (reac-pw README, Known issues): a desk's boxes enrol first,
reac-pw joins last, and reac-pw leaves the segment while a box reboots. A real fix needs the
slave to see the box's absence and drop itself, or a recording mode that never courts at all
(passive on a mirror) — both open. Evidence: reac-captures `courtship-trial-2026-09-12/`.
