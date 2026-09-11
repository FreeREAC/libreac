# libreac

Two C libraries for Roland REAC (*REAC Exposed Audio Communications*, EtherType `0x8819`):
`libreac`, the protocol, and `libreac-transport`, the sockets and threads that carry it.
Both build from this one repository and one release tarball.

## libreac — the protocol

libreac owns what the bytes mean, in both directions and for both roles.

**Wire format.** The REAC frame is rate-invariant: 40 channels × 12 samples × 3 bytes of
24-bit audio, braided into a channel-pair byte layout that is the same on the wire whichever
way it is read. `reac_braid_pos()` (`<reac/reac_braid.h>`) is the one oracle for that layout —
`static inline`, so encode and decode both call it and cannot drift apart. `reac_s24le_to_f32()`
/ `reac_f32_to_s24le()` (`<reac/reac_sample.h>`) are the exact-inverse sample codec.
`reac_decode()` / `reac_frame_inspect()` (`<reac/reac_decode.h>`) read the master's 40-channel
downstream broadcast; `reac_upstream_channels()` / `reac_upstream_decode()`
(`<reac/reac_upstream.h>`) read a stagebox's narrower upstream return, sized to the box's own
input count; `reac_braid_encode()` / `reac_downstream_build()` (`<reac/reac_encode.h>`) write
audio back onto the wire in both directions. `reac_decode_plain_le()` reads the pre-0.5.0
plain-LE layout; it is a diagnostic for historical captures, not a layout the wire ever carried.
`reac_frame_is_reac()`, `reac_frame_counter()`, `reac_counter_gap()`, `reac_rate_snap()` and
`reac_detect_rate_fd()` recognise a frame, read its sequence counter and detect its sample rate
from live capture. `reac_frame_clean_len()` strips the +2-byte Ethernet FCS residue some
captures carry after the end marker — not a protocol field, never emitted.

**Control plane.** The REAC conversation itself: how one endpoint pairs with another. The
32-byte control block, its two nested checksums and the box-model matrix
(`reac_ctrl_build_*`, `reac_master_stamp`), head-amp records (`reac_headamp_*`,
`reac_ports_parse`), box identity, and the master/slave establishment, hunt and arbitration
state machines (`<reac/reac_link.h>`, `reac_hunt`, `reac_master_fsm`, `reac_grant`). See
[`docs/REAC-CONTROL-PLANE.md`](docs/REAC-CONTROL-PLANE.md) for the pairing sequence as measured
on real boxes, and [`docs/layering.md`](docs/layering.md) for what belongs here and what does
not.

libreac is IO-free, allocation-free and clock-free: no socket, no thread, no `SCHED_FIFO`. That
is `libreac-transport`'s job.

## libreac-transport — sockets, pacer, threads

The pieces of a REAC endpoint that move frames but carry no opinion about their meaning:
AF_PACKET RX/TX over a lock-free SPSC ring (`reac_rx`, `reac_tx`, `reac_ring`), the SCHED_FIFO
cadence pacer and its clock discipline (`reac_pacer`), network interface enumeration and link
state (`reac_ifscan`, `reac_linkmon`, `reac_ifname`), VLAN sub-interface mint/adopt/release on a
trunk port (`reac_topo`, `reac_vlan`), a segment's identity and lock (`reac_segment_ident`,
`reac_seglock`), the layered-config precedence and the one door to `SCHED_FIFO` (`reac_conf`,
`reac_rt`), and the slave/master establishment orchestration that drives libreac's protocol
state machines (`reac_slave` joining, `reac_pacer` mastering). Its public headers carry no socket type in a call shape, so a
future backend other than userspace AF_PACKET could implement the same API; it holds no Linux
capability itself, since a library cannot — the binding process keeps `CAP_NET_RAW` /
`CAP_NET_ADMIN` and this library runs inside it. See
[`docs/design/specs/2026-09-11-reac-transport-library.md`](docs/design/specs/2026-09-11-reac-transport-library.md)
for what moved here from where, and what is still open (two headers still vendored from their
prior home, five structs that still expose a raw `fd`).

## Who links these

`reac-pw`, the PipeWire-native REAC endpoint, links both: `libreac-transport` for the wire and
`libreac` underneath it for what the frames mean. `reac-aes67`, the REAC→AES67 bridge, links
`libreac` alone — it has no need of the transport layer's threads or pacer.

## The three paces

REAC runs at exactly one of three packet rates, all carrying the same 40×12×3-byte frame
shape: 44.1 kHz, 48 kHz and 96 kHz. A master announces which one it is running in the control
block's `console_field` byte (`cfea` offset 19 in the wire spec) — 0 for 48 kHz, 1 for 96 kHz,
2 for 44.1 kHz — and `reac_pace_code()` (`transport/src/reac_pacer.c`) is the one place that
byte is derived, from the running rate in packets per second (≥ 8000 pps → 96 kHz, ≤ 3700 pps →
44.1 kHz, otherwise 48 kHz). See `spec/reac.ksy` in the
[reac-protocol](https://github.com/FreeREAC/reac-protocol) repository for the field itself.

## Build

A hand-kept Makefile, no build system to configure.

```
make                                            # libreac.a
make test                                       # libreac's own suite
make transport REACPW_INCLUDE=<reac-pw>/src     # libreac-transport.a
```

`libreac-transport` builds standalone for everything except two headers
(`reac_pacer.h`, `reac_role_swap.h`) that still `#include` two pure-declaration headers from
`reac-pw`'s tree (`reac_rate_cfg.h`, `reac_role_cfg.h` — see the design spec above for why).
`REACPW_INCLUDE` points the build at a `reac-pw` checkout's `src/` for those two; unset, every
other object still builds and only those two fail, loudly, at compile time.

## Packaging

`packaging/build-rpm.sh` builds every `*.spec` under `packaging/` — today `libreac.spec` and
`libreac-transport.spec` — from the one tarball `packaging/make-tarball.sh` produces, so both
RPMs always ship the same source snapshot. `packaging/publish-repo.sh` assembles the shared
dnf tree; see `.github/workflows/release-rpm.yml` for how a tagged release runs that dispatch.

## Tools

Under `tools/`, built with `make wire-tools` (the six analysis tools) or named individually:

- `corpus_check` — decode a capture corpus with this build and report what libreac made of it;
  `--self-test` / `--self-test-audio` prove the corruption-detection arm can itself go red.
- `headamp_trace FILE.pcap` — every head-amp record in a capture, in order, with source MAC and
  truncation.
- `wire_census FILE.pcap` — who talks on a segment and in what frame shapes.
- `ctrl_delta FILE.pcap` — which control-block bytes change, per talker and message kind.
- `upstream_watch FILE.pcap AA:BB:CC:DD:EE:FF` — every byte one box's own frames change, classed
  by how often.
- `slotmap_watch FILE.pcap` — the sliding slot-map window unrolled into per-slot state.
- `seq_gaps FILE.pcap` — per-talker frame-counter holes, the control for any "nothing was sent"
  claim.
- `conformance-headamp-base.sh` — a source-shape gate: the head-amp base must have exactly one
  derivation in the code (the announced strap), never a per-width table.
- `run-corpus.sh` — decode the full FreeREAC capture corpus and diff the result against
  `tests/corpus-baseline.txt`; the corpus itself is private and not in this repository.
- `gen-facts-header.py` — regenerate `tests/reac_facts_assert.h` from a `reac-protocol`
  checkout's `spec/protocol-facts.yaml`; run by the Makefile, not by hand.

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
