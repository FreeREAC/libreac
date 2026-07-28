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
- **Constants** — EtherType, frame geometry, and the settled facts (96 kHz is
  40 ch / 8000 pps, *not* channel-halving).

The wire-format reference these come from is
[reac-protocol](https://github.com/FreeREAC/reac-protocol).

## Scope: RX/measure today, TX is future work

libreac is **receive- and measure-oriented** right now: validate a frame, read its
counter, detect/snap the rate from cadence. Everything it models is the master's
**downstream broadcast** — the fixed 40-channel program frame (1492 B = 50 + 1440 + 2),
rate-invariant audio with the sample rate carried by the packet rate. That frame is
well-characterised, so RX/measure is solid ground (mostly verified on the wire).

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

## Build

Native (static lib + tests):

    make        # libreac.a
    make test   # build + run the unit tests

OpenWrt (shared lib + dev headers): the package recipe is `openwrt/libreac/`. A
dependent package declares `DEPENDS:=+libreac` and `#include <reac/reac.h>`.

## API

See [`include/reac/reac.h`](include/reac/reac.h) — plain C with simple types, so it is
also straightforward to bind from other languages (e.g. a thin `ctypes` wrapper for
the Python tools) if cross-language consistency or speed ever calls for it.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
