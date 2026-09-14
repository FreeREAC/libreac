# Capturing a linked, silent box — what the push has to be, and what it already is

Status: **measured 2026-09-14**; the burst-rate defect below is FIXED and gated
(`tests/test_master_capture.c`). It shipped once as 1.1.1 and was WITHDRAWN for an
ABI break it carried — §5. Re-released as 1.1.2, ABI-identical to 1.1.0.

Source of truth for every number here:
`reac-captures/desk-arrival-q4-2026-09-14/desk-arrival-slice.pcap` (M-200
`00:40:ab:c9:cc:03` unplugged and replugged at 44.1 kHz; S-4000S-3208
`00:40:ab:c4:08:bc` linked to the switch throughout, its cable never touched; our
`.12` a tap, no frame of ours in the file) and §6 of
`reac-captures/m200-enrol-441k-2026-09-13/analysis.md`.

## 1. The premise this repo carried was false

`include/reac/reac_master.h` said, for over a month, that *"the box only
cold-connects on a real link-down/up (§13b)"*, and the daemon's PROBING watchdog
told the operator to bounce the box's PHY. Both describe a box that does not
exist.

A box whose desk vanishes sends ONE link-4 re-assert at +88.4 ms, keeps streaming
full-width audio into the void for 5.629 s, then goes silent — and stays silent.
It never bounces its own link. When the desk came back it was captured in 3.380 s
with nothing touched, and **what captured it was one COMPLETED scene transfer**.

The capture carries its own negative control: the desk's FIRST push after
returning was the tail of a transfer it had begun while unplugged — 338 MIDDLE
chunks and a LAST with **no FIRST** — and the box did not answer it. The next push
was complete (FIRST declaring `0x22c8`, 341 chunks, LAST) and the box was
transmitting **8.355 ms after the last chunk**.

**The box answers with its state-4 commit report, not a cold connect.**
`cdea 01 03 0010 84`, and its `cdea 04 03` burst comes 1.698 s later — only after
the desk's ENROLL group map. A master that waits for a `04 03` JOIN from a warm
box waits behind its own grant.

## 2. What was compared, and what was found

Our master's push was compared against the desk's **byte for byte**, with the
capture as the oracle:

| | desk | libreac `reac_master` | verdict |
|---|---|---|---|
| FIRST block[0:16] | `01 01 0018 00 22c8 "1234" 01 00 00 00 04` | identical | same |
| MIDDLE block | `01 00 001a 00 …` ×341 | identical | same |
| LAST block[0:16] | `01 02 000e 00 03 00 00 00 01 00 00 00 00 00` | identical | same |
| declared total | `0x22c8` | `0x22c8` | same |
| chunk count | 341 | 341 | same |
| scene BODY, 8904 B | reassembled from the capture | `reac_scene_placeholder` + our MAC + revision `0x02` | **0 bytes differ** |
| cfea while hunting | width `0x08`, count `0x0000`, pace `0x02` | identical bar the MAC | same |
| ENROLL group map | `04 02 41 41 41 41 00 00 00 00 00 c3` | identical | same |
| FILLER descriptor | `00 <current chunk's checksum>` ×16 | identical | same |
| transfer period | 2.6957 s | 2.6945 s (`cycle_len`) | same |
| **HEAD → LAST** | **2511 slots** (counter 46839 → 49350) | **2392 slots** | **DEFECT** |

So the content was never the problem. The one divergence is the **burst rate**.

### The burst rate is a ratio, and it was being truncated

The desk emits 500 chunks a second. `src/reac_master.c` computed
`probe_stride = fps / 500` and placed chunk *k* at `k * probe_stride`. That divides
exactly at 48 kHz (8) and 96 kHz (16) — **the only two rates every golden was read
at** — and truncates 7.35 to 7 at 44.1 kHz, where the truncation is paid 341 times:
the whole transfer went out in 2392 slots against the desk's 2511, 4.7 % fast.

Fixed by keeping the ratio: `burst_slot()` places chunk *k* at the ROUNDED
`k * fps / 500`, and `probe_stride` (still the LAST frame's offset behind the final
chunk) is rounded rather than truncated. At 44.1 kHz that reproduces 2511 exactly.
At 48 kHz and 96 kHz the emitted stream is **byte-identical** — verified by
diffing the whole 9-second emit sequence (kind, template index, scene step) before
and after: same md5 at 4000 and 8000 fps, different at 3675.

**Whether that 4.7 % is what a real box refuses is NOT established here.** It is
the only measured divergence, and it is now gone; the rig decides.

## 3. What the FSM already did

Nothing in the establishment path needed changing, and this was checked rather than
assumed. Driven against a fake box written from the capture's own bytes — silent
until a complete transfer arrives, answering with the capture's commit-report block,
never cold-connecting — `reac_master` goes
`PROBING → GRANTING → ESTABLISHED` at 44.1, 48 and 96 kHz:

- `reac_ctrl_classify_box_frame` reads the commit report as `REAC_M_RX_BOX_CONFIG`;
- `reac_ports_parse` sizes the box from it at 32 in / 8 out, strap `0x00`;
- `EDGE[REAC_M_PROBING][REAC_M_EV_CONFIG_EARLY]` takes the warm relink — sabotaged
  to a HOLD, the test goes red, which is the proof that arm is load-bearing;
- the ENROLL group map follows the report inside the desk's measured ~210 ms;
- the box's late `04 03` burst is a no-op — it does not restart the dwell.

## 4. What the rig still owes

One run, and it is pass/fail with no interpretation:

> Stop the daemon for 30 s beside a linked box. Confirm the box is SILENT on the
> wire (a capture with a known-non-empty control in the same command — an empty
> tcpdump and a broken tcpdump look identical). Start the daemon. **PASS:
> `rx_box_frames > 0` and the box enrolled, with no cable touched.** FAIL: it sits
> at `rx_box_frames=0`.

If it fails, the next measurement is where the box sends its commit report: the
capture cannot separate "the box answers whoever completed the push" from "the box
answers the master MAC it still remembers", because the desk that returned had the
same MAC it left with. Ours will not. That is the first thing to look for in the
trace, and it is a question no reading of the code can settle.


## 5. What 1.1.1 cost, and the ratchet that now stands in the way

The fix above shipped as libreac 1.1.1 with the chunk index kept in a CURSOR — one
`int burst_k`, added to `struct reac_master` between `probe_stride` and
`burst_end`. Entirely internal state, and an ABI break:

| | v1.1.0 | 1.1.1 |
|---|---|---|
| `sizeof(struct reac_master)` | 14456 | 14464 |
| `sizeof(struct reac_pacer)` | 24312 | 24320 |
| `reac_master.headamp_src` (a POINTER) | 14304 | 14308 |
| `reac_master.box_mac` | 14344 | 14348 |
| `reac_pacer.recognized_box` (a POINTER) | 15232 | 15236 |

`LIBREAC_ABI` stayed 3 and both sonames stayed put, so the dynamic linker handed
the new library to the installed reac-pw 1.0.4, built against 1.1.0. Its periodic
timers then read those pointers from the wrong offsets and dereferenced them:
**SEGV about 7 s after every start, 99 restarts on the live rig** before the
rollback.

`struct reac_master` is public and reac-pw EMBEDS it in `struct reac_pacer`. That
is the whole of it — and nothing in the suite could see it, because every test is
rebuilt against the headers it is testing and therefore agrees with itself
perfectly. `make test` was green in the same commit.

**The fix is ABI-identical.** The chunk index is a pure function of the slot, so
it is computed (`burst_index_at`) rather than stored, and the struct does not grow
at all. All 61 public structs now match v1.1.0 byte for byte — verified against
the headers inside the built `libreac-devel-1.1.2` RPM, not only against the tree
— and the emitted frames are byte-identical to 1.1.1's at all three rates.

**The ratchet.** `tests/abi-layout.inc` records sizeof, `_Alignof` and all 566
member offsets of those 61 structs, generated from DWARF by
`tools/gen-abi-layout.py`; `tests/test_abi_layout.c` re-measures them with plain
`offsetof` in `make test` and inside the RPM's `%check`. Re-making the 1.1.1
mistake reports 128 differences and exits 1; bumping `LIBREAC_ABI` without
regenerating the table exits 1 with the reason. Both were run.

**Reproduced off the rig, and cured.** `tools/fake_box` is a wire-level
linked-silent box built from this capture's bytes — it answers only a completed
push, with the commit report, and never cold-connects. Driven against reac-pw
v1.0.4 on a veth inside a private network namespace with its own PipeWire:

| library run against | result |
|---|---|
| 1.1.0 (what it was built against) | 30 s, `ESTABLISHED`, `rx_box_frames=25686` |
| **1.1.1** | **SEGV (139) after 3 s**, in a periodic timer dereferencing the box-model pointer |
| 1.1.2 (this fix) | 30 s, `ESTABLISHED`, `rx_box_frames=25604` |

The crash frame here is `on_autodetect_timer` (reac-pw `src/main.c:618`) rather
than the rig's `sink_publish_link_props`; it is the same fault — a periodic timer
dereferencing a pointer read at a shifted offset — reached by whichever timer
fires first after the box is recognized. Said plainly rather than dressed up as
the rig's exact frame.

That run also settles §4's question in the affirmative on a wire, though not on
the rig: reac-pw's own master, with no cable touched and no `04 03` JOIN first,
recognized the box as `S-4000S (32 in / 8 out)` and established. What the rig
still owes is the same thing with a REAL box, whose commit report we cannot yet
prove is addressed to whoever completed the push rather than to the master MAC it
remembers.
