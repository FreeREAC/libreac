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
