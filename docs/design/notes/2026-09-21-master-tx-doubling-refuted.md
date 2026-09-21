# "A master emits every downstream frame twice" — REFUTED: mirror twins, already resolved

- **Governs nothing.** The protocol facts are in `include/reac/reac.h`,
  `reac-protocol/wire-format.md` and `reac-protocol/spec/reac.ksy`; where this note and
  they disagree, they win.
- **Subject:** reac-pw issue #92 / branch `fix/master-tx-frame-doubling` (tip `ff6b437`,
  2026-08-20), which proposed a `REAC_PACER_TX_REPS 2` in the pacer so the master would
  emit each downstream frame twice, back-to-back, same bytes and same counter.
- **Verdict: REFUTED — nothing ported.** The "doubling" is the capture tap, not the desk.
  libreac's pacer keeps ONE emission per slot and there is no `REAC_PACER_TX_REPS`.

## The prior resolution, by SHA

1. **libreac `0b65341`** (2026-07-29, PR #15 `fix/plus2-is-fcs-residue`, merge `45fa22e`) —
   the diagnosis: the +2 bytes are the frame's own Ethernet FCS residue, and "the variable
   is the capture rig — mirroring both RX and TX of one port, so a transiting frame is seen
   twice (same source MAC, same counter, identical payload), one copy clean and one with
   the residue". It is `include/reac/reac.h:35-47` today.
2. **reac-tools PR #4 `fix/mirror-twin-dedup-rate`, merge `33a9fbd`** (2026-07-29) — the
   mechanical criterion and the fix: `3186324` "drop the mirror twin before anything is
   measured", `4cc4a87` "frame geometry + `clean_payload_len`", `4a2352e` "measure the
   deduplicated stream". A twin is same MAC, same counter, equal `clean_payload_len()`
   bytes, the pair differing in LENGTH (1492/1494 downstream, 628/630 upstream). Its corpus
   sweep found 0 adjacent same-counter pairs differing in any byte and 0 byte-identical
   pairs whose counters differ — so the dedup cannot drop a distinct frame.
3. **reac-captures `8bd985e`, `bce2036`, `caf8b20`** (2026-08-22) — the corpus cleanup:
   strip a mirrored capture to one copy per frame and prove it; a third of the files was
   duplication, and `verify_unique` judges the u16 counter STEP between consecutive frames
   of one source (0 = duplicate, 1 = intact). Mirrored captures are labelled as such in the
   corpus (`…-mirror__…`, `ba9b5aa`, `2ae73a0` "mirror of the desk port", `248de70` "tap
   proof … on the mirror"), and `reac-captures/MANIFEST.md` states the consequence: "a rate
   read as packets-per-second off a mirrored capture is 2x wrong".
4. **reac-captures `2ccb62c`** (2026-09-14) — the mirror path distorts measurements beyond
   frame counts: a direct-link control captured on the TX NIC reads p99 131 us, while "the
   mirror path inflated late/catch-up 10-300x".

Both of #92's goldens are mirrored taps
(`m200i-s4000s-48k-mirror__matrix-m200-s4000-coldconnect-2026-07-24.pcap`,
`m5000-s4000s-96k-mirror__matrix-m5000-s4000-unit1-coldconnect-2026-07-11.pcap`), measured
raw, three weeks after the dedup that exists for exactly this. PR #93 was withdrawn and
#92 closed by its author on 2026-08-20, four minutes after the push, for this reason.

## Independent confirmations

- **The clean copies never repeat a counter.** Deltas over clean-length frames only
  (`52 + 36n`), consecutive records within 5 ms, 2026-09-21: M-200 @48k mirror golden
  1455 frames / **0** delta-0; M-5000 @96k mirror goldens 16995 and 34683 frames / **0**;
  the S-4000S in the same file 1000 / **0**; and the non-mirrored captures
  (`m200-probing-nobox`, `m200-s1608-realbox-establish`, `m300-s1608-coldconnect`) 0 as
  well. Every delta-0 in those files sits in the RESIDUE copies. Counter advance reads
  ~4000/s at 48 kHz and 8000/s at 96 kHz — `REAC_MODE_48K` / `REAC_MODE_96K`, `pps =
  rate / 12`, nothing to reconcile.
- **The grammar.** `spec/reac.ksy` is built from real traffic and encodes no frame
  doubling; the residue is deliberately NOT a field, so a serializer generated from it
  cannot emit one.
- **The link budget.** 96 kHz already runs `8000 x (1492 + 24) x 8` = 97.0 Mbit/s, about
  saturating 100BASE-TX (`wire-format.md`). A doubled 96 kHz master would need 194 Mbit/s,
  which the link cannot carry.
- **The rig.** The live console enrols and runs all three boxes — S-1608, S-0808,
  S-4000S-3208 — against the present once-emitting pacer. A box that needed pairs could
  not lock to it. (The S-4000 join #92 blamed on single emission was root-caused on the
  declaration path instead: libreac `7702d6e`, `09d5710`, `b50070c`, `2920484`, `c8aed34`;
  reac-pw `74ee4f8`.)

## Disposition

`transport/src/reac_pacer.c` unchanged. reac-pw's `fix/master-tx-frame-doubling` is a
stale artifact of a withdrawn PR and carries nothing libreac wants. An inline
(non-mirrored) tap plus a box's own received-frame count would close the wire question
from the box's side; the verdict does not wait on it.
