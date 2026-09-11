# A second library, `libreac-transport`, for the pieces of reac-pw that never touch PipeWire

Status: ruled by the operator, 2026-09-11; partly implemented (see §6 for what landed this lane
and what is still open).

- **Author:** Pau Aliagas <linuxnow@gmail.com>
- **Supersedes:** [`docs/layering.md`](../../layering.md)'s "Three layers" table, the row
  `transport | sockets, the SCHED_FIFO pacer, RT threads, PipeWire nodes | reac-pw, permanently`
  and the sentence "The transport layer never moves." That note is about the *control-plane
  conversation* (the FSM) — its readiness gates and sequencing for THAT question still hold — but
  its transport row is flatly wrong after this ruling and `docs/layering.md` now carries a
  pointer to this file at its top instead of restating the row.
- **Conforms to, does not reopen:** `libreac`'s own `docs/REAC-CONTROL-PLANE.md` (one library,
  two headers, for the *protocol* — frame layout and control-record meaning share one buffer and
  one model matrix; unaffected by this split, which is about a different seam: sockets/threads
  vs. PipeWire binding, not wire-format vs. control). The openmixer specs
  `2026-08-23-reac-trunk-vlan-daemon.md` (segment = interface, autodetect, VLAN-via-kernel) and
  `2026-08-20-reac-master-arbitration.md` (one master per segment) are the governing law for the
  *behaviour* the moved code implements; this spec only moves where that behaviour lives, and
  changes none of it.

## 0. The ruling, verbatim

> "once it finishes, move it to its own library. I think that the lib belongs to the libreac
> repo, right?" — yes: a second library, `libreac-transport`, **in the libreac repo**, depending
> on `libreac`. `libreac` stays the pure protocol (frames, `reac.ksy`, state machines).
>
> "kmod is very serious, we might do it optionally" — the API is **backend-agnostic**: no
> `AF_PACKET`/socket type appears in the public header. Userspace is the shipping backend; a
> kernel-module backend (`reac-kmod`, today a frame counter) may implement the same shape later.

## 1. The three layers, restated with the new boundary

| layer | owns | lives |
|---|---|---|
| **wire format** | byte layouts, codecs, frame geometry, builders/parsers | `libreac` |
| **control plane (protocol)** | the REAC conversation's *meaning*: establishment FSM, head-amp records, chanmap, hunt/arbitration | `libreac` (since 0.8.0, `docs/REAC-CONTROL-PLANE.md`) |
| **transport** | sockets, the SCHED_FIFO pacer, RT threads, interface/VLAN scanning, the ring, segment locking | **`libreac-transport`** (new, this spec) |
| **binding** | PipeWire nodes, props, the graph-facing config surfaces, `main()`'s node half | `reac-pw` |

The transport layer *does* move — what stays permanently in `reac-pw` is the **binding**, not
"everything outside libreac" as the old table implied. `reac-pw`'s job shrinks to: turn transport
events into PipeWire graph facts, and turn PipeWire prop writes into transport calls.

## 2. What moves, file for file

Explicitly named (16 files, ~5 700 lines below libreac-transport's cut, verified zero `pw_`/
`spa_` references except the one comment in `reac_rx.c`):

`reac_ifscan` (450), `reac_topo` (435), `reac_vlan` (319), `reac_slave` (1090), `reac_pacer`
(1863), `reac_tx` (99), `reac_rx` (521, one `pw_loop_add_signal` **mentioned in a comment only** —
removed), `reac_linkmon` (262), `reac_segment_ident` (120), `reac_seglock` (66), `reac_role_swap`
(76), `reac_ring` (157), `reac_rt` (124), `reac_pace_watch` (101), `reac_ifname` (142),
`reac_conf` (154).

**Entailed, not independently decided:** `reac_mac.c/h` (36 lines, zero `pw_`/`spa_` refs) is
`#include`d by four of the sixteen (`reac_slave.c`, `reac_pacer.c`, `reac_tx.c`,
`reac_segment_ident.h`) and by two files that stay (`main.c`, `reac_sink_node.c`) — it is
connective tissue the named list cannot compile without, so it moves too; both sides consume it
as a public transport header from here on.

reac-pw's *local* `reac_link.h`/`reac_link.c` ("is there a cable" — `reac_link_carrier()`, 26
lines, distinct from libreac's protocol-level `<reac/reac_link.h>` pairing API) is used only by
`reac_pacer.c`. It moves too, **renamed to `reac_carrier.c/h`** so the transport tree never holds
two files named `reac_link.h` for two unrelated things — the function name is unchanged.

**Not moved, and why:**

- `reac_gain.c`, `reac_lat.c`, `reac_node_ensure.c`, `reac_node_recover.c`, `reac_watch.c`,
  `reac_knock.c`, `reac_box_pin.c` — zero `pw_`/`spa_` refs like the rest, but not named in the
  ruling and not entailed by a moved file's `#include`; they concern node-rebuild decisions,
  gain ramping and latency smoothing on the audio path, or segment-serving policy layered above
  the transport primitives. Left for a follow-up lane to classify one at a time rather than swept
  in under this spec's authority.
- `reac_rate_cfg.h` / `reac_role_cfg.h` stay in `reac-pw` whole (their `.c` implements a
  `spa_pod`-parsing function, explicitly kept per the ruling), **but two moved headers still
  `#include` them** (`reac_pacer.h` → `reac_rate_cfg.h`, `reac_role_swap.h` → `reac_role_cfg.h`),
  for pure enums/decls (`REAC_RATE_REFUSE_*`, `REAC_CFG_ROLE_*`) that sit beside the one
  `spa_pod`-touching function in the same file. This is a real seam: **building
  `libreac-transport` standalone needs `reac-pw`'s private headers on the include path**
  (`REACPW_INCLUDE` in the Makefile, §5). Splitting those two headers into a pure half (moves)
  and a binding half (stays) is the honest fix and is **open**, not done here — recorded so it is
  not silently re-discovered.
- `main.c`'s transport half (segment/socket/hunt orchestration, ~48 `pw_` refs mixed through
  3 210 lines because the event loop IS `pw_loop` — every `on_*_io`/`on_*_timer` callback takes a
  `struct pw_loop *`) is **not split in this lane**. See §7.

## 3. The public API

Backend-agnostic: no `AF_PACKET`, no `sockaddr_ll`, no socket `int` in any signature a caller
sees for lifecycle purposes (the RX/TX modules keep sockets as an internal implementation detail
of the userspace backend; a kmod backend would satisfy the same call shape over `ioctl`/netlink
to the module instead). What today's move actually exposes, unchanged in signature from what
`reac-pw` already called:

- **Segments appear/disappear** — `reac_ifscan_*` (interface enumeration + link state),
  `reac_topo_*` / `reac_vlan_*` (VLAN sub-interface mint/adopt/release, per the trunk-VLAN spec),
  `reac_segment_ident_*` (a segment's bus+address-derived name, MAC as one atomic), each carrying
  interface name, VLAN id where present, and the box's model/MAC once `reac_slave`/`reac_pacer`
  have identified it.
- **Frames in/out per segment** — `reac_rx_*` (the feeder thread, the RX ring), `reac_tx_*` (the
  AF_PACKET emission), `reac_ring_*` (the lock-free SPSC ring between them).
- **The hunt/election as the transport's own state machine, driven by the protocol library** —
  `reac_slave_*` (the box-establishment FSM) and `reac_pacer_*` (the master pacer + establishment)
  both call `libreac`'s `reac_hunt`/`reac_link`/`reac_ctrl`/`reac_fsm` for *what a frame means* and
  own only *when to send and at what cadence*.
- **Clock/pacing hooks the binding needs** — `reac_pacer_clock_publish`/`_clock_tick` (unchanged),
  through which `reac-pw`'s PipeWire-driver-clock reading feeds the pacer's DLL.
- **The capability handoff** — `libreac-transport` performs no `CAP_NET_ADMIN`/`CAP_NET_RAW`
  preflight or `prctl`; a library cannot hold a Linux capability, only the process can. `reac-pw`
  keeps `capability_preflight()` in `main.c` and the RPM's `%caps` line; the transport calls that
  need the raw socket / VLAN-admin rights simply run inside the caller's already-capable process,
  same as today.
- **Refusal codes** — every transport decision keeps returning an `enum`, never a bool: this move
  changes no function's error-reporting shape.
- **`reac_conf_*` / `reac_rt_*`** — the layered-config precedence law and the one door to
  `SCHED_FIFO`, unchanged.

**The OS handle is opaque (0.9.1, closing the seam a review found the same day):** review of
the 0.9.0 move found a raw `int fd` struct member in five installed headers (`reac_tx`,
`reac_pacer`, `reac_slave`, `reac_seglock`, `reac_ifscan`), and the fix found two more
(`reac_linkmon`, and the topo tap's open/next/close signatures). All seven now hold ONE
`struct reac_handle *` (`include/reac/transport/reac_handle.h`, an incomplete type; complete
only in `transport/src/reac_handle_priv.h`), NULL meaning not open. The library allocates it
in the object's own open/claim (control plane) and frees it in close/release; RT paths read
the descriptor through a private inline accessor — one pointer read, no allocation. What a
caller may still ask for is a POLLABLE descriptor for its event loop (`reac_ifscan_fd`,
`reac_linkmon_fd`, `reac_topo_tap_fd`) — that is the reactor seam of §7, and on Linux any
backend answers it. `reac_seglock_init`/`reac_seglock_held` replace the two things `reac-pw`
and its tests did to the lock's descriptor by hand. Struct layouts changed, so
`libreac-transport` moves to `.so.2`.

No event-loop abstraction is introduced by this increment (§7) — everything above is called
synchronously or from a caller-owned thread, exactly as `reac-pw` calls it today; only the
`main.c` orchestration wiring these into `pw_loop` remains to be extracted.

## 4. Versions

- `libreac` 0.8.1 → **0.9.0** — a new library is the one middle-digit bump this deserves, not a
  patch: `libreac-transport.so`/`.a` is a wholly new build product beside `libreac.so`, new
  headers under `include/reac/transport/`, a new `.spec` subpackage, and a floor every downstream
  consumer must declare explicitly rather than inherit for free.
- `libreac` 0.9.0 → **0.9.1**, `libreac-transport` abi 1 → **2** — the opaque handle above
  changes every transport struct's layout; a consumer built against 0.9.0 must rebuild, and
  the soname says so. `libreac.so` itself is unchanged.
- `reac-pw` 0.5.11 → **0.5.12** — follows the handle (the topo tap and the seglock init call
  sites, four pacer tests, two seglock tests); floor `libreac-transport >= 0.9.1`.
- `reac-pw` 0.5.10 → **0.5.11** — no behaviour change, sources removed in favour of a link
  dependency; a middle-digit bump is not warranted because nothing observable from outside the
  binary differs (§6's before/after test counts are the evidence for that claim, not an
  assertion).

## 5. Build shape

`libreac`'s Makefile globs `src/*.c` into `libreac.a`/`libreac.so`; `libreac-transport` is a
**second, parallel object family**, not folded into the same glob (mixing them would make every
transport file part of libreac's own soname and defeat the point of a second library):

```
transport/src/*.c     -- the moved sources, plus reac_mac.c, reac_carrier.c
transport/src/*.h     -- their headers (local, quoted #include between siblings)
```

installed public headers under `include/reac/transport/*.h` (mirrors `include/reac/*.h`'s own
shape), a `libreac-transport.pc` generated the same way `libreac.pc` is (inline in the RPM
`%install`, §8), `Requires: libreac`.

`REACPW_INCLUDE` (Makefile variable, default empty) — when set, added as `-I$(REACPW_INCLUDE)` so
`transport/src/reac_pacer.h`'s `#include "reac_rate_cfg.h"` and `reac_role_swap.h`'s
`#include "reac_role_cfg.h"` resolve against a checked-out `reac-pw/src`. Building
`libreac-transport` with `REACPW_INCLUDE` unset still succeeds for every object that does not
reach those two headers; the two that do fail loudly with "file not found" rather than silently
skipping — this is the §2 seam made visible at build time instead of hidden.

## 6. What is proven this lane, mechanically

- Every moved file keeps its own test file; pcap-replay tests move with the hunt/establishment
  code (`test_reac_slave.c`, `test_reac_pacer*.c`, `test_reac_courtship.c`, `test_reac_ifscan.c`,
  `test_reac_topo.c`, `test_reac_linkmon.c`, `test_reac_ifname.c`, `test_reac_conf.c`,
  `test_reac_rt.c`, `test_reac_ring.c`, `test_reac_link.c`/renamed `test_reac_carrier.c`,
  `test_reac_tx.c` — the whole-binary/veth tests (`hearing-finds-a-segment.sh`,
  `box-master-slave-join.sh`, `box-master-rejoins.sh`, `link-up-reestablishes.sh`,
  `iface-vanish-exits.sh`, `link-edge-observed.sh`) stay in `reac-pw` because they drive the
  **linked binary**, not a library in isolation — the behaviour they gate is unaffected by which
  `.a` supplies the symbols.
- `tests/test_reac_hunt.c` §K/K2 (already in `libreac` since the 0.8.0 control-plane move) is
  unaffected by this spec — it tests protocol-layer hunt/arbitration, not the transport files
  moving here; named in this spec only to record that it was checked and does not move again.
- Both repos' suites run before and after with the **same counts** (§ report has the numbers);
  `reac_hearing_finds_a_segment` is a known slow flake — rerun alone if red, per its own test
  comment.
- The `reac-pw` binary's `--help`/env vocabulary (`REAC_ROLE_<segment>`, `REAC_RATE`,
  `REACPW_CLOCK_*`) is unchanged — a link-only move changes no flag or env var.
- Sabotage: one boundary is broken on purpose (the binding reaching into a transport internal)
  and must fail the **build**, not just a link — see the report for which symbol and how.

## 7. What is explicitly NOT done this lane, and why

`main.c`'s split (segment/socket/hunt orchestration vs. the node half) is **deferred**. Every
transport callback in `main.c` (`on_sniff_io`, `on_topo_io`, `on_autodetect_timer`,
`hearing_hunt`/`hearing_yield`/`hearing_apply`, `listener_open`/`_close`/`_reopen_*`) takes a raw
`struct pw_loop *` and calls `pw_loop_add_io`/`pw_loop_add_timer` directly — the transport
orchestration is not merely *located* in a PipeWire-touching file, it is **written against
PipeWire's own reactor** as its I/O multiplexer. Extracting it needs a small event-source
abstraction (`reac_transport_reactor` — register an fd, register a timer, cancel either) that
`reac-pw` implements over `pw_loop` and a future embedder could implement over `epoll` directly;
without one, moving these functions verbatim would make `libreac-transport` depend on
`libpipewire`, which defeats the point of a userspace/kmod-agnostic library. Designing that
reactor shape is a task of its own, not a mechanical `git mv`, and is left as the next increment
this spec authorizes rather than one it also executes.

## 8. Testing / packaging law

- `libreac`: `make test` — unchanged suite plus whatever `transport/` gains its own harness for
  later; this lane's transport files keep running under `reac-pw`'s `meson test` for now (§6),
  since `libreac`'s Makefile has no meson-style per-target runner yet.
- `reac-pw`: `meson test` — same test names, same counts, sources reduced.
- RPM: `packaging/libreac-transport.spec` (new, alongside `libreac.spec`, same soname/pkgconfig
  pattern) built from the same tarball process; `reac-pw`'s spec gains
  `BuildRequires: pkgconfig(libreac-transport)` alongside its existing `libreac` floor.
