# libreac

The shared **REAC protocol library** for the [FreeREAC](https://github.com/FreeREAC)
C tools — *REAC Exposed Audio Communications*. One canonical home for the REAC
(EtherType `0x8819`) facts and helpers that `reac-aes67`, `reac-repacer`, and future
C tools all need, so they aren't duplicated or allowed to drift.

## What it provides

- **Mode descriptors** — `REAC_MODE_{44K1,48K,96K}` and `reac_mode_for(rate)`. The
  frame is rate-invariant (40 ch × 12 samples × 3 B); the rate is the packet rate.
- **Rate detection** — `reac_rate_snap(pps)` (pps → 44100 / 48000 / 96000) and
  `reac_detect_rate_fd(fd, window_ms)` (measure the live packet rate on an AF_PACKET
  capture and snap it).
- **Frame helpers** — `reac_frame_is_reac()`, `reac_frame_counter()`,
  `reac_counter_gap()` (16-bit wrap-aware loss).
- **The braid layout oracle** — `reac_braid_pos()` (`<reac/reac_braid.h>`): the
  channel-pair byte map of the audio region, the REAC wire format in **both**
  directions. `static inline`, so real-time encode and decode both call it. Do not
  copy this byte map anywhere; consumers call the oracle.
- **The sample codec** — `reac_s24le_to_f32()` / `reac_f32_to_s24le()`
  (`<reac/reac_sample.h>`): the one s24-LE ↔ float pair, exact inverses. Both
  directions live together so the round-trip contract cannot drift.
- **Downstream decode** — `reac_frame_inspect()` / `reac_decode()`
  (`<reac/reac_decode.h>`), plus capture and pcap sources for offline work.
- **Upstream decode** — `reac_upstream_channels()` / `reac_upstream_decode()`
  (`<reac/reac_upstream.h>`): the stagebox return, box-width sized.
- **Constants** — EtherType, frame geometry, and the settled facts (96 kHz is
  40 ch / 8000 pps, *not* channel-halving).

The wire-format reference these come from is
[reac-protocol](https://github.com/FreeREAC/reac-protocol).

## Scope: the layout oracle for both directions; emission lives in reac-pw

libreac owns **what the bytes mean**, in both directions and for both roles. It
validates and decodes frames, measures the wire (counter, loss, rate from cadence),
and supplies the **layout and sample oracles the encoders use** — `reac_braid_pos()`
is `static inline` precisely so a real-time TX path can call it, and
`reac_sample.h`'s s24-LE ↔ float pair is bidirectional by construction (the two are
exact inverses, which is the point of keeping them in one header).

What it does **not** do is put frames on the wire. Builders, the control-plane
checksums and counter stamping live in reac-pw — but they take their byte layout
from here, so the layout has exactly one home. "libreac decodes, reac-pw emits" is
the split; it is *not* that emission is unsolved. reac-pw's master role emits REAC
that real Roland stageboxes lock to.

The master's **downstream broadcast** is the fixed 40-channel program frame
(1492 B = 50 + 1440 + 2), rate-invariant audio with the sample rate carried by the
packet rate.

A stagebox's **upstream return** (box → master) is a different, narrower frame,
now **decoded here too** (`<reac/reac_upstream.h>`, resolved on the rig — reac-pw
task #108 + the S-4000 OHRCA captures):

- It carries the box's own input count, not 40 — a **variable, even, box-dependent
  channel count** (S-0808 → 8 ch/340 B, S-1608 → 16 ch/628 B, S-4000 → 32 ch/1204 B).
- Audio is the **channel-pair byte braid** (`<reac/reac_braid.h>` — the single
  layout oracle, with the full evidence trail; the braid is the wire format in
  both directions). The plain-LE `reac_decode()` path is retained unchanged as
  the diagnostic/legacy downstream decode — see the contested note in its header.
- The channel map is **plain ascending** (input N = wire channel N−1) — the once-
  suspected FPGA permutation was disproved by the captures.
- OHRCA-generation gear appends a **+2 CRC trailer** after the end marker in both
  directions; `reac_frame_clean_len()` is the one home for stripping it.

Frame *emission* (builders, control-plane checksums, counter stamping) still lives
in reac-pw; its encoders take the byte layout from this library's braid/sample
oracles, so the layout knowledge has exactly one home.

Where the boundary runs — and why the establishment FSM has **not** followed the
wire format here yet — is recorded in [`docs/layering.md`](docs/layering.md),
together with the concrete gates for moving it.

## Build

Native (static lib + tests):

    make        # libreac.a
    make test   # build + run the unit tests

OpenWrt (shared lib + dev headers): the package recipe is `openwrt/libreac/`. A
dependent package declares `DEPENDS:=+libreac` and `#include <reac/reac.h>`.

## API

Seven headers under [`include/reac/`](include/reac), each carrying its own evidence
trail in the header comment — read those before trusting any summary, including this
one:

| header | what lives there |
| --- | --- |
| `reac.h` | modes, rate snap/detect, frame helpers, geometry constants |
| `reac_braid.h` | the audio-region byte map — the single layout oracle, both directions |
| `reac_sample.h` | s24-LE ↔ float, the one conversion pair |
| `reac_decode.h` | downstream frame inspect + decode |
| `reac_upstream.h` | stagebox return decode, box-width sized |
| `reac_capture.h` | AF_PACKET capture |
| `pcap_source.h` | offline pcap source |

Plain C with simple types, so it is also straightforward to bind from other languages
(e.g. a thin `ctypes` wrapper for the Python tools) if cross-language consistency or
speed ever calls for it.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
