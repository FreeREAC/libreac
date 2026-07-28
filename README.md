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
- **Encode** — `reac_braid_encode()` / `reac_downstream_build()`
  (`<reac/reac_encode.h>`): audio *onto* the wire. The braided audio region in
  both directions (the exact inverse of `reac_upstream_decode`), and the whole
  1492-byte master downstream broadcast frame. Pure — caller's buffer, no
  allocation, no IO, safe from a real-time thread.
- **Constants** — EtherType, frame geometry, and the settled facts (96 kHz is
  40 ch / 8000 pps, *not* channel-halving).

The wire-format reference these come from is
[reac-protocol](https://github.com/FreeREAC/reac-protocol).

## Scope: the wire format in both directions; IO and handshake live in reac-pw

libreac owns **what the bytes mean**, in both directions and for both roles, and
now in both *senses*: it validates and decodes frames, measures the wire (counter,
loss, rate from cadence), and **builds** them. Encoding a frame is the same
statement about the wire format that decoding makes, read backwards, so the two
belong together — `reac_braid_encode()` is literally the inverse of
`reac_upstream_decode()`, and `reac_downstream_build()` is the 40-channel master
broadcast that `reac_decode()`'s counterpart reads.

What libreac does **not** do is touch a socket, a clock or a protocol state
machine. Staying in reac-pw, deliberately:

- **AF_PACKET emission** (`reac_tx_emit`) and the SCHED_FIFO cadence pacer — IO
  and timing; libreac is IO-free and allocation-free by design.
- **The control-plane builders** `reac_ctrl_build_*` (cold-connect,
  config-announce, upstream/flood FILLER, head-amp) and `reac_master_stamp` —
  their 32-byte control block, its two nested checksums and the box-model matrix
  are *handshake state* tied to the master/slave FSM, not layout. They call
  `reac_braid_encode()` for their audio region and own everything else.

That split is also why there is **no whole-frame upstream builder here**: on a
real stagebox every box → master frame that carries audio is *also* a control
frame, so the only layout-pure, separable part of the upstream emit is its audio
region — which is `reac_braid_encode()`, and which is the same braid the
downstream uses. A `reac_upstream_build()` would have had to import the
handshake, so it was deliberately not written.

The master's **downstream broadcast** is the fixed 40-channel program frame
(1492 B = 50 + 1440 + 2), rate-invariant audio with the sample rate carried by the
packet rate.

A stagebox's **upstream return** (box → master) is a different, narrower frame,
decoded here too (`<reac/reac_upstream.h>`, resolved on the rig — reac-pw
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

## Build

Native (static lib + tests):

    make        # libreac.a
    make test   # build + run the unit tests

OpenWrt (shared lib + dev headers): the package recipe is `openwrt/libreac/`. A
dependent package declares `DEPENDS:=+libreac` and `#include <reac/reac.h>`.

## API

Eight headers under [`include/reac/`](include/reac), each carrying its own evidence
trail in the header comment — read those before trusting any summary, including this
one:

| header | what lives there |
| --- | --- |
| `reac.h` | modes, rate snap/detect, frame helpers, geometry constants |
| `reac_braid.h` | the audio-region byte map — the single layout oracle, both directions |
| `reac_sample.h` | s24-LE ↔ float, the one conversion pair |
| `reac_decode.h` | downstream frame inspect + decode |
| `reac_upstream.h` | stagebox return decode, box-width sized |
| `reac_encode.h` | braided encode + the downstream frame builder |
| `reac_capture.h` | AF_PACKET capture |
| `pcap_source.h` | offline pcap source |

Plain C with simple types, so it is also straightforward to bind from other languages
(e.g. a thin `ctypes` wrapper for the Python tools) if cross-language consistency or
speed ever calls for it.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
