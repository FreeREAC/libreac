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
- `reac-pw` `tests/test_reac_courtship_backoff.c` — the real `reac_slave_open/start` over a veth
  pair against a non-granting master, TX timestamps recorded off the wire.
