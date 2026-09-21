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

## How this should have been caught

We own the protocol: a `.ksy` grammar built from real traffic, and a corpus whose mirror
twins were stripped a month before this branch was written. Nothing in either was
consulted — the claim "wire-verified" was believed through two triage passes on the
strength of a commit message, and no one parsed a capture until now. A static reading
cannot settle a wire question, and prose in `reac.h` did not stop it twice.

**Built, not owed any more:** `tests/test_wire_invariants.c`, in `make test`. Real traffic
is checked in (`tests/wire-invariants.inc`, 3800 records from the two mirrored captures #92
measured plus one off a plain NIC, via `tools/gen-wire-invariants.py`), and the gate asserts
one emission per slot, the counter advance against `reac_rate_snap`, the 100BASE-TX bound,
and residue-only-in-mirrored-captures — with a positive control (mirrored captures must
yield twins), a negative control (a synthetically doubled stream must be caught) and a
refusal to pass on an empty or short fixture. Measured: M-200 48k mirror 594 emissions at
4000 pps → 48 kHz, 48.5 Mbit/s; M-5000 96k mirror 700 at 8000 pps → 96 kHz, 97.0 Mbit/s;
M-200 48k clean 1000 at 4000 pps; 1294 twins dropped, 0 violations, 800 violations caught
on the doubled stream.

## The residue census: it is the tap, in every capture we own

Counted 2026-09-21 over all 104 pcaps in the corpus, VLAN tag honoured (a mirror on a trunk
hands every frame back tagged, `include/reac/pcap_source.h`; a first pass that did not strip
the tag read 31 files as empty and would have been a blind scan). Positive control: 0 of 104
files read zero REAC frames.

| tap | files | clean frames | residue frames |
|---|---:|---:|---:|
| mirror-tagged name | 50 | 372,068 | 624,948 |
| VLAN trunk — the desk-port mirror | 25 | 2,739,781 | 3,719,553 |
| **plain NIC (`-clean` / `-direct`)** | **25** | **592,762** | **0** |
| untagged, no VLAN | 4 | 3,446 | 41 |

Not one residue-length frame in 592,762 frames captured off a plain NIC. The 41 sit in two
untagged files, `captures/role-m-boot-20260831-223528-ctrl.pcap` (23 clean / 22 residue) and
`captures/role-rival-arrives-20260831-215902-ctrl.pcap` (23 / 19) — a 1:1 split, which is
the mirror signature itself; their filenames simply do not declare a tap. Not evidence of a
wire variant, and not proof of one either, so they are named here rather than rounded away.

So the +2 is a fact about the capture path and about nothing else. The grammar already
refuses to model it as a field (`spec/reac.ksy:45-76`, "explained, stripped, NOT modelled"),
but it still computes it: `instances/has_fcs_residue` and `clean_len` (`spec/reac.ksy:342-356`,
with `spec/protocol-facts.yaml:113,236` pinned to them). Those two instances, and libreac's
`reac_frame_clean_len()` / `REAC_FRAME_BYTES_OHRCA`, are INGEST vocabulary living in the
protocol's vocabulary. Moving them is a reac-protocol change and cannot ride this branch;
the inventory for whoever takes it: ingest keeps it — `transport/src/reac_rx.c:162,347-355`
(the live duplicate guard), `transport/src/reac_tap.c` (`prev_clean_len`); the parsers that
currently strip it themselves — `src/reac_upstream.c:18`, `src/reac_disco.c:145` — are the
ones that should be handed clean bytes instead, and `tests/test_braid.c:87-91` is the
fixture that pins the strip.

**TAKEN AND DONE, 2026-09-21** — ruled by the operator the same day ("the FCS residue
provably does not even appear on a normal REAC network and should be removed from the
grammar"). reac-protocol `lane/residue-out-of-grammar`: `has_fcs_residue` and `clean_len`
are out of the grammar, `len_audio` is `raw_len - 52` so a buffer carrying anything past
the end marker is REFUSED rather than tolerated, the FCS_RESIDUE fact is retired (a
capture-path artifact is not a protocol fact) and FRAME_OVERHEAD is re-pinned to
`instances/len_audio/value`. libreac `lane/residue-ingest-only`: the doors strip
(`reac_rx`'s loop, `reac_tap`'s survey, `reac_pacer_rx_ingest`, `reac_hunt_observe`,
`tools/corpus_check`) and the parsers refuse a residue length (`reac_upstream_channels`,
and `reac_disco_classify` reads the geometry off `len` as given). `tests/test_rx_twin.c`
holds the door with a no-twin control; `tests/test_braid.c`'s fifth block holds the
parsers. `reac_frame_clean_len()` and `REAC_FRAME_BYTES_OHRCA` stay exactly where they
were — they are ingest's, and now they are only ingest's.

## Disposition

`transport/src/reac_pacer.c` unchanged. reac-pw's `fix/master-tx-frame-doubling` is a
stale artifact of a withdrawn PR and carries nothing libreac wants. An inline
(non-mirrored) tap plus a box's own received-frame count would close the wire question
from the box's side; the verdict does not wait on it.
