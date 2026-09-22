# Deciding what a wire is, and courting the box on it, belongs to the library

Status: ruled by the operator, 2026-09-22; this lane implements the first two modules (§4) and
inventories the rest (§6).

- **Author:** Pau Aliagas <linuxnow@gmail.com>
- **Continues, does not reopen:** `2026-09-11-reac-transport-library.md`. That spec drew the
  seam (wire format / control plane / transport / binding) and left seven files "for a follow-up
  lane to classify one at a time rather than swept in under this spec's authority" — naming
  `reac_knock.c` first among them. This is that follow-up lane, and it classifies against the
  same table rather than inventing a second one.

## 0. The ruling, verbatim

> "The box enrolment belongs 100% to libreac and libreac-transport, not to reac-pw." — and, as a
> fundamental law: "anything related to REAC protocol and REAC transport belongs to the lib;
> reac-pw deals only with enrolled REAC nodes."

What forced it: reac-pw 1.0.22 (`lane/court-the-remembered-box`, 4e94d8f..f2b087f) put wire
re-decision logic INTO the daemon — `src/reac_tapwait.{h,c}`, `src/reac_knock.c`, and the
hunt's re-decision at `src/main.c:4281`. The rule those modules carry ("a wire heard for a
moment and then silent is decided again, not pinned") is a property of the wire state machine,
not of a PipeWire binding, and the wire state machine is `reac_hunt` — which already lives in
libreac and already had a door for it.

## 1. The three-line design gate

**1. Which primitive?** `reac_hunt` (libreac, `include/reac/reac_hunt.h`). It is the module that
decides which end of the desk↔stagebox pairing this segment takes on a wire nobody configured,
and it already carries `int silence_proven` and the entry point `reac_hunt_silence_proven()`,
whose docstring cites `reac_knock.h` by name — a header that did not exist in this repo. The
primitive was here; only the two inputs that feed it were in the wrong repo.

**2. Extend the vocabulary first?** Yes, and minimally: two PUBLIC headers under `include/reac/`
(`reac_knock.h`, `reac_tapwait.h`) with the `reac_` prefix the rest of the surface uses. No new
concept is minted — both are moved verbatim, including their `struct` and `enum` names, so
nothing anywhere has to be re-decided or re-derived.

**3. Why isn't the knock a `reac_hunt` member?** Because `struct reac_hunt` is PUBLIC and its
layout is in the ABI ratchet (`tests/test_abi_layout.c`, 61 structs / 578 offsets). Folding the
observation's clock into it would move offsets and cost an ABI break for a pure relocation. A
new struct beside it is a new struct, and new structs are free. The two objects have different
lifetimes as well: one knock lives per unserved wire and is thrown away when the licence is
granted, while the hunt outlives it.

## 2. The home: `libreac`, not `libreac-transport` — and why the brief's first answer is wrong

The lane brief proposed `transport/src/` with headers under `include/reac/transport/`, "beside
`reac_topo`/the hunt in libreac-transport". The premise is factually wrong about one of the two
neighbours: **`reac_hunt` is not in libreac-transport.** It is libreac core —
`src/reac_hunt.c`, `include/reac/reac_hunt.h` — and has been since the 0.8.0 control-plane
move. The 2026-09-11 spec's own layer table says why:

| layer | owns | lives |
|---|---|---|
| control plane (protocol) | the REAC conversation's *meaning*: establishment FSM, head-amp records, chanmap, **hunt/arbitration** | `libreac` |
| transport | sockets, the SCHED_FIFO pacer, RT threads, interface/VLAN scanning, the ring, segment locking | `libreac-transport` |

Four mechanical facts settle it:

1. **Neither module names a single transport noun.** No socket, no thread, no ifindex, no VLAN
   id, no netdev. `reac_knock.c` includes `<string.h>` and nothing else; `reac_tapwait.c`
   includes its own header and nothing else. Both are a clock and a verdict.
2. **`REAC_TAPWAIT_NS` *is* `REAC_HUNT_WINDOW_NS`**, a libreac-core constant, and its test
   asserts the identity rather than a literal. Putting the guard in transport means core
   defines the bar and transport enforces it — one rule split across a library boundary and a
   soname.
3. **The knock's output is a libreac-core call**: `reac_hunt_silence_proven()`. The dependency
   runs knock → hunt, and libreac cannot depend on libreac-transport (transport `Requires:
   libreac`; the reverse is a cycle).
4. **`reac_hunt.h` already cites `reac_knock.h`** three times (lines 103, 104, 147) as if it
   were a sibling header. It was written expecting this home. Honouring that is "unify, never a
   second copy"; transport would have been the second copy's address.

The tap the tapwait waits *for* is transport's (`reac_topo`), but the tapwait reaches no topo
symbol: the caller hands it two integers the tap happens to produce. A guard on the hunt is a
hunt thing, not a tap thing.

**Recorded as a divergence from the brief, not absorbed silently.** If the operator wants the
transport address instead, it is one `git mv` plus an include path, and this section is the
argument to overturn.

## 3. What this is NOT

Not a behaviour change. The two `.c` files move byte-identical but for their `#include` line;
the two headers move byte-identical but for their include guard's neighbours and the one
`#include <reac/reac_hunt.h>` that was already spelled that way. `reac-pw` calls the same three
functions with the same arguments in the same order at the same two call sites. The rig proof is
therefore a NO-CHANGE proof: the same 10-restart script must read the same as it did on 1.0.22.

## 4. The public API (added by this lane)

`include/reac/reac_knock.h` — the masterless observation, the licence to drive a wire:

```c
#define REAC_KNOCK_LISTEN_NS (500ULL * 1000000ULL)
enum reac_knock_state { REAC_KNOCK_LISTENING, REAC_KNOCK_PROVEN, REAC_KNOCK_CANCELLED };
enum reac_knock_act   { REAC_KNOCK_ACT_NONE, REAC_KNOCK_ACT_DRIVE };
struct reac_knock { ... };                                    /* new struct */
void  reac_knock_init (struct reac_knock *k, uint64_t now_ns);
void  reac_knock_heard(struct reac_knock *k, uint64_t now_ns);
enum reac_knock_act reac_knock_step(struct reac_knock *k, uint64_t now_ns);
```

`include/reac/reac_tapwait.h` — how long a sighting the tap has not classified binds the hunt:

```c
#define REAC_TAPWAIT_NS REAC_HUNT_WINDOW_NS
struct reac_tapwait_in { int tapped; unsigned long untagged;
                         uint64_t last_heard_ns; uint64_t now_ns; };   /* new struct */
int reac_tapwait_binds(const struct reac_tapwait_in *in);
```

Five added symbols, two added structs, two added enums. **No existing struct or symbol moves or
changes size**, so `LIBREAC_ABI` stays 4 and `tests/test_abi_layout.c` stays green unchanged —
which is the mechanical arm of that claim, not an assertion about the diff.

## 5. Versions

- **`libreac` 1.3.2 → 1.4.0.** A new public header family is SURFACE growth, and the 1.3.0
  precedent (`reac_tunables.h`) is exactly this shape: added symbols only, `LIBREAC_ABI`
  unchanged, minor bump. `libreac-transport` follows the version string (one tarball) with no
  soname move — it gains no symbol here.
- **`reac-pw` 1.0.22 → 1.0.23**, floor `libreac >= 1.4.0`. Behaviour unchanged; sources removed
  in favour of a link dependency. The last digit, per the versioning rule.

## 6. The rest of the inventory — what still sits on the wrong side of the seam

Measured 2026-09-22 on `reac-pw` main (7c862a0), by reading each module's own header and
counting `pw_`/`spa_` references. `(a)` = already a call into the library, correct as is.
`(b)` = logic that belongs in a library, scheduled as its own lane.

| module (reac-pw) | lines .c+.h | what it decides | verdict | destination | effort |
|---|---|---|---|---|---|
| `reac_knock` | 56+126 | a wire observed masterless may be driven | (b) | **libreac** — DONE, this lane | — |
| `reac_tapwait` | 26+78 | an unplaced sighting binds the hunt, bounded | (b) | **libreac** — DONE, this lane | — |
| `reac_watch` | 80+129 | a SERVED segment can still be re-decided | (b) | libreac, beside `reac_hunt` | small — pure, own test, no `pw_` |
| `reac_wake` | 102+126 | when a master may break its link to wake a dropped box | (b) | libreac-transport (the link is transport's) | small — pure, own test |
| `reac_link_budget` | 26+74 | how much of a physical link a REAC master costs | (b) | libreac (`rate/12 × 1492` is wire arithmetic) | small |
| `reac_declared_vlan` | 82+68 | which VLAN segments the operator declared vs. the wire mentioned | (b) | libreac-transport, beside `reac_vlan`/`reac_topo` | small |
| `reac_headamp_state` | 109+133 | what this daemon asserts on the head amp, and why a write was refused | (b) | libreac (head-amp records are control plane) | medium — couples to `reac_headamp_prop` (binding), needs a split |
| `reac_segconf` | 551+230 | the operator's one override file; the only thing that can pin a segment | (b) | libreac-transport, unified with `reac_conf`'s precedence law | large — two config doors today |
| `reac_box_pin` | 50+59 | `--box MODEL[:LABEL]` resolved against libreac's model table | split | resolve half → libreac; the CLI flag stays | small |
| `reac_qdisc` | 288+123 | the daemon owns the qdisc on the device it binds | (a)+(b) | mechanism already `reac_etf_qdisc` (transport); the rtnetlink STATS read is a second door | small |
| `reac_knobs` | 184+75 | every `REACPW_`/`REAC_` knob, discovered and published | (a) | **stays** — the library reads no environment (`test_no_getenv_conformance`) | — |
| `reac_roster`, `reac_roster_node` | 352 | every segment as one node's properties | (a) | **stays** — PipeWire node properties | — |
| `reac_gain`, `reac_lat`, `reac_node_ensure`, `reac_node_recover`, `reac_sink_*`, `reac_source_*`, `reac_headamp_prop`, `reac_rate_cfg`, `reac_role_cfg` | ~4 400 | graph PCM, latency, node lifecycle, SPA pod parsing | (a) | **stays** — binding | — |
| `main.c` hearing/hunt orchestration | ~900 of 5 813 | `on_sniff_io`, `on_topo_io`, `on_autodetect_timer`, `hearing_hunt`/`_yield`/`_apply`, `listener_open`/`_close` | (b) | libreac-transport, **blocked** on the reactor abstraction | large — 2026-09-11 §7 defers it by name; every callback takes a `struct pw_loop *` |

**And the finding the file list does not show.** 43 of reac-pw's 72 C unit tests
(`tests/test_*.c`) include no reac-pw header at all — they are LIBRARY tests living in the
binding's suite: `test_reac_hunt`, `test_reac_hunt_captures`, `test_reac_arbitration`,
`test_reac_slave`, `test_reac_pacer*`, `test_reac_topo`, `test_reac_ifscan`, `test_reac_rx_*`,
`test_reac_tx`, `test_reac_courtship`, `test_reac_master*`, `test_reac_grant*`,
`test_reac_headamp*`, `test_reac_box_*`, and the rest. So reac-pw's "103 ok" is mostly a
measurement of libreac, taken in the wrong repo, where a libreac change cannot see it go red.
Moving those test files is its own lane and is worth more than several of the module moves
above: it is what makes libreac's own `make test` the authority for libreac's own behaviour.

## 7. Testing law for this lane

- `libreac`: `make test` gains `test_reac_knock` and `test_reac_tapwait`, the moved files
  unchanged in their assertions. `make test-transport` and the ABI layout ratchet stay green
  with no edit — the ABI arm is the proof that §4's "no existing struct moves" is a measurement.
- `reac-pw`: the two moved test files are DELETED with their sources, and one thin BINDING test
  replaces them — `tests/test_reac_enrolment_binding.c` — which links the real library and
  asserts the two rules the daemon depends on (a cancelled observation re-opens; a stale
  unplaced sighting stops binding). Its job is not to re-test the library's arithmetic but to
  prove that the library reac-pw actually links still carries the rule.
- Sabotage, run both ways: delete the re-decision in `libreac/src/reac_knock.c` (the
  `REAC_KNOCK_CANCELLED` re-open) and `libreac`'s `test_reac_knock` must red AND reac-pw's
  binding test must red against the sabotaged library. A guard that survives its own sabotage is
  decoration.
- Live, after both releases: the main session's `scratchpad/reac-pw-restart-proof.sh`, 10
  restarts, with the SAME result as 1.0.22 — this lane claims no behaviour change, so a
  difference is a regression, not a feature.
