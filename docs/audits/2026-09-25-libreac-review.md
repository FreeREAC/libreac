# libreac review — 2026-09-25

Read-only review of `FreeREAC/libreac` at `main` = `ee205b6` (1.5.0, LIBREAC_ABI 4).
Nothing was run against a desk, box, cluster or PipeWire daemon.

**Scope:** `src/` and `include/reac/` for decode, capture, clock, arbitration,
ctrl/ctrlblk, boxreg, braid and cfg; their tests; the build. There is no
`meson.build`: the build is the top-level `Makefile`, and that is what was reviewed.

**Governing design read first:** `docs/design/specs/*`, `reac-protocol`
`spec/reac.ksy` and `spec/protocol-facts.yaml` (cloned read-only). Where a finding
touches the spec, the spec clause is cited. M4 is the one finding that argues from the
spec: the code contradicts a settled clause, so the question is which of the two moves.

**Baseline:** `make test` is green on `ee205b6`. `test_sniffer_binds_first` prints
SKIPPED in this container (no `ip`).

**Proof:** `make review-2026-09-25` builds and runs every proof test below. The target
is deliberately **not** part of `make test`. Each proof test has a control arm that must
pass first, and exits 2 (NOT A RESULT) if it does not. So a red result means the finding
reproduced, not that the harness broke. On `ee205b6` all 7 proofs are red and every
control arm passes. `test_review_rate_detect` was run 5 times and was red all 5 times.

| Severity | Count |
|---|---|
| HIGH | 0 |
| MEDIUM | 7 (M1–M7, each with a failing test) |
| LOW | 9 (L1–L9) plus the declare-once inventory (D) |
| Discarded after re-verification | 11 |

---

## MEDIUM

### M1 — `reac_detect_rate_fd` counts both directions and snaps 48 kHz to 96 kHz
- **Where:** `src/reac.c:93-132` (the counter is `src/reac.c:117`). The caller is
  `transport/src/reac_rx.c:484`.
- **Mechanism:** every frame that passes `reac_frame_is_reac()` is counted, whatever
  its geometry or direction. The rate is `(frames-1)/span`. Some sockets hear both
  directions of one session. Examples: a host that transmits on the same NIC
  (`reac_capture_open` does not set `PACKET_IGNORE_OUTGOING`), a segment where an
  ungranted box floods its broadcast FILLER, or a mirrored port. On those sockets
  the count is 2 × 4000 frames/s, and `reac_rate_snap(8000)` returns 96000.
- **Failure scenario:** a 48 kHz master is heard together with its S-1608's return,
  1492 B + 628 B at 4000/s each. `reac_rx_open()` then sets `sample_rate = 96000`,
  sizes the ring for 96 kHz and runs the graph at double rate. Nothing reports it
  until `reac_pace_watch` notices.
- **Proof:** `tests/test_review_rate_detect.c`. An AF_UNIX datagram pair is fed on
  absolute CLOCK_MONOTONIC deadlines. Control arm, downstream only: detects 48000.
  Verdict arm, both directions: **detects 96000**.

### M2 — a newly selected clock reference is reported LOCKED before it is measured
- **Where:** `src/reac_clock.c:309-343`. The branch that is missing is at `:338`.
- **Mechanism:** when the source changes, `in_band` and the stability series are
  reset, but `state` is left alone unless the same call carries a measurement. The
  `else if` at `:338` only moves UNLOCKED/HOLDOVER to LOCKING. `reac_pacer`
  (`transport/src/reac_pacer.c:1478-1490`) passes `have_measurement = 0` whenever
  the newly selected source has no fresh stamp.
- **Failure scenario:** the master is LOCKED to the box slope, then a PHC becomes
  available. The PHC outranks the box, so it is selected. The state stays LOCKED
  with zero PHC samples, and `reac_clock_disc_describe` says
  "locked to NIC/external PHC". `reac_pace_from_clock()` publishes pace `phc`,
  and `reac_clock_disc_period_ns` keeps applying the box's correction under the
  PHC's name.
- **Proof:** `tests/test_review_clock.c`. Control: 8 in-band box samples → LOCKED.
  Verdict: after the switch with no sample, the state is **LOCKED** and the pace is
  **phc**.

### M3 — `reac_ctrl_identity_reply` accepts a reply that fails both checksums
- **Where:** `src/reac_ctrlblk.c:337-362`. The caller is
  `transport/src/reac_pacer.c:577`, which runs before
  `reac_ctrl_classify_box_frame` at `:583`.
- **Mechanism:** the function checks the tag, the model/cmd bytes, the SysEx length
  and f0/f7. It never checks the outer block checksum (sum-to-0 over `[18:50]`) or
  the inner record checksum (sum-to-0x80 over TAG..CKSUM). A real reply satisfies
  both. The one door that checks the block checksum is the classifier, and the
  pacer calls it only after the identity has been ingested. The reply is also taken
  before the Roland-OUI and own-echo filters. This contradicts `src/reac_ctrl.c:50`:
  "an invalid checksum is corrupt — never … evidence of anything".
- **Failure scenario:** one flipped payload byte in an S-1608 firmware reply
  (2.200 → 2.500) is ingested. `reac.box-firmware` then publishes 2.500.
- **Proof:** `tests/test_review_identity_cksum.c`. Control: the captured S-1608 record
  (matrix `cc0016`) verifies and extracts. Verdict: the corrupted copy fails both
  checksums, is **extracted (1)**, and ingests as fw_milli **2500**.
- **Why the existing tests missed it:** `tests/test_identity.c:68` builds its fixture
  with a stand-in `0x7f` checksum and no block checksum, so it could not see this.

### M4 — box-side doors emit and accept the 40-channel (1492 B) downstream geometry
- **Spec:** `reac.ksy` `num_channels`: "40 is the downstream broadcast; an even
  2..38 is a box's upstream return". The same file's "The role is the geometry"
  says a 1492 B frame is a master. libreac restates this law in
  `reac_frame_is_master_downstream` (`include/reac/reac.h:80`),
  `reac_upstream_channels` (`src/reac_upstream.c:28`) and `reac_braid.h:52`
  ("2..38").
- **Where the code contradicts it:**
  - the `fr4000` row (`src/reac_ctrlblk.c:623`, in_ch 40);
  - `reac_box_model_upstream_width`, which bounds `in_ch > REAC_MAX_CHANNELS`
    (`src/reac_box_synth.c:266`);
  - `ctrl_emit_as`, which bounds `n_ch > REAC_MAX_CHANNELS`
    (`src/reac_ctrlblk.c:893`);
  - `reac_boxreg`'s `width_ok`, which allows `nch <= r->fabric` = 40
    (`src/reac_boxreg.c:55`).
- **Failure scenario:** a box built from `fr4000`, or asked for a 40-wide filler,
  emits 1492 B frames. A libreac master reads them as a master downstream, so
  `reac_rival_kind_from_channels(40)` gives a rival **DESK**. It cannot decode the
  audio either: `reac_upstream_decode` returns -1.
- **Proof:** `tests/test_review_box_width.c`. Control: a 16-channel filler
  round-trips. Verdict: the `fr4000` width is 40, its frame is master-downstream,
  `reac_upstream_channels` returns -1, the 40-wide filler builds, and boxreg
  accepts 40.
- **Question for the integrator:** the operator's 2026-09-17 experiment ("emulate
  a 40 channels input … box") and the settled geometry law cannot both hold.
  Either the row caps at 38, or the spec changes first.

### M5 — `reac_boxreg_declare` signed overflow lets a pinned base land outside the fabric
- **Where:** `src/reac_boxreg.c:64` (`base + nch > r->fabric`, in `int`).
- **Mechanism:** a base from the CLI near `INT_MAX` overflows the sum, which is
  undefined behaviour and wraps negative in practice. The bound therefore passes,
  `range_taken` finds no overlap, and the box is registered.
- **Failure scenario:** `base = INT_MAX-4, nch = 8` is registered at audio slot
  **2147483643**. The RX then indexes the fabric with that value.
- **Proof:** `tests/test_review_boxreg.c`. Control: base 36 with nch 8 is refused.
  Verdict: base `INT_MAX-4` is **accepted**.

### M6 — `reac_decode_plain_le` reads past the frame for a mode wider than the region
- **Where:** `src/reac_decode.c:96-117`. The header claim that is wrong is
  `include/reac/reac_decode.h:76`: "a linear index needs none".
- **Mechanism:** `struct reac_mode` is public and caller-supplied.
  `reac_decode()` refuses `nch*ns*3 > REAC_AUDIO_BYTES` (`src/reac_decode.c:70`),
  but the plain-LE twin has no size check. For `{48000, 42, 12}` it reads up to
  frame offset 1561 of a 1492-byte buffer.
- **Proof:** `tests/test_review_decode_plain_le.c`. The frame ends at a PROT_NONE
  guard page and the call runs in a forked child. Control: `reac_decode` returns
  -1. Verdict: `reac_decode_plain_le` gets **SIGSEGV**.

### M7 — `reac_cfg.h` is a dead declaration; the consumer's copies restate it and disagree
- **Where:** `include/reac/reac_cfg.h` (the whole file). No file in `src/`,
  `transport/` or `include/` reads any of its macros. The installed
  `include/reac/transport/reac_pacer.h:45` includes the **vendored reac-pw**
  `reac_rate_cfg.h`, which, together with `reac_role_cfg.h`, restates every key
  under other names: `REAC_CFG_PROP_RATE`, `REAC_PROP_RATE_REFUSED`,
  `REAC_ROLE_STATE_REESTABLISH_PENDING` and the rest.
- **Declare-once violation:** `reac_cfg.h:8` says "ONE DECLARATION, BOTH SIDES",
  and openmixer's TS mirror is pinned byte-for-byte against this file. That pins
  it to a file the daemon does not read.
- **Already drifted:** `REAC_CFG_REFUSED_NONE` is `""` (`reac_cfg.h:122`). The
  vendored headers answer `"none"` when nothing is refused (`reac_rate_cfg.h:83`,
  `reac_role_cfg.h:118`), so openmixer and reac-pw already disagree on the idle
  refusal value. See also L1.
- **Proof:** `tests/conformance-review-cfg-declared-once.sh`, a source-shape arm in
  the style of `tools/conformance-*.sh` with a planted good/bad pair. Result:
  **22 macros read nowhere, 13 restated, sentinel mismatch.**

---

## LOW
Each LOW has a reason and a location; none carries a failing test.

- **L1 — `reac_cfg.h` prose contradicts its own values.** `reac_cfg.h:33-34` says
  refusals are "one of the three short kebab strings below". There are four, and
  they are snake_case. `:42` names a refusal code `not-in-list` that is declared
  nowhere; the real code is `not_closed`.
- **L2 — `reac_capture.h` misstates its contract.** `:35` says "0 is never
  returned" in blocking mode, but `src/reac_capture.c:65` returns 0 on EINTR in
  blocking mode, deliberately, so a stop flag can be checked. `:7` advertises a
  source-MAC filter that does not exist.
- **L3 — capture socket options.** `reac_capture_open` opens the socket without
  `SOCK_CLOEXEC` (`src/reac_capture.c:36` passes flags 0), so the fd leaks into any
  exec'd child. It also does not set `PACKET_IGNORE_OUTGOING`, which is one of M1's
  feeders.
- **L4 — `reac_boxreg` has no departure API.** The comment at
  `src/reac_boxreg.c:43` ("Fills gaps left by a departed box") describes a path
  nothing can reach. If a unit is swapped on a full fabric (5 × S-0808), the new
  MAC is refused (-1) until `reac_boxreg_init`, and that call also drops the
  operator's pinned declarations.
- **L5 — `reac_boxreg_add` with a zero MAC.** `find` returns -1 for the zero MAC
  (`src/reac_boxreg.c:24`), so each call allocates a new entry. That entry still
  reads as an unbound pre-declared slot (`:92`), so repeated calls fill the
  registry.
- **L6 — a false checksum comment.** `src/reac_ctrlblk.c:1019` says the captured
  0013 block "sums to 0xfe mod 256 — NOT sum-to-0". It sums to **0x00**. The
  0013/0016/001a blocks all close to 0. The "emitted RAW" rationale rests on a
  measurement that is wrong. The bytes emitted are unaffected.
- **L7 — tests that cannot fail, or do not exist.**
  - `tests/test_box_table.c:206`: `reac_box_model_by_channels(40)` never returns
    NULL or a derived row, so the assertion is always true.
  - `include/reac/reac_slots.h:55` claims MUTATION-CHECKED via
    `tests/test_reac_grant.c`, which is not in this repo. Widening
    `REAC_AUDIO_FABRIC_SLOTS` to 48 leaves `make test` **green** (run here).
  - `reac_clock`/`reac_dll`, `reac_arbitrate`, `reac_boxreg` and `reac_grant`
    have no behavioural test in this repo, only ABI layout.
  - `test_sniffer_binds_first` exits 0 on SKIP (`:184`). It is loud, but CI cannot
    tell "skipped" from "passed". Exit 77, or a separate status, would let it.
- **L8 — pcap truncation reads as EOF.** `src/pcap_source.c:70` skips an oversize
  record by `fseek`. If that record is truncated, the seek goes past EOF and the
  next read returns 0 (EOF) instead of -1, so a cut capture ends quietly. This is
  at the edge of scope.
- **L9 — enum-indexed tables without a static assert.** `KIND_NAME`
  (`src/reac_ctrlblk.c:217`) and `REAC_CLOCK_SRC_COUNT` (`reac_clock.h:85`) are
  indexed by enum with no `_Static_assert` tying them to the enum. A new
  `reac_ctrl_kind` would silently print "?". Neither is wrong today.

## D — declare once, use everywhere (operator ruling 2026-09-25)

Each row names the one declaration that should be used, and the copies of it.

| Declaration (use this) | Hardcoded copies / dead twin |
|---|---|
| `REAC_ETHERTYPE` (`reac.h:19`) | bytes `0x88,0x19` at `src/reac.c:48`, `reac_decode.c:21`, `reac_upstream.c:40`, `reac_capture.c:70`, `reac_ctrl.c:28`, `reac_ctrlblk.c:27,254,682`; `ETH_P_REAC 0x8819` again at `reac_capture.c:21` |
| `REAC_HDR_COUNTER_OFF` / `reac_frame_counter()` (`reac.h:27`, `reac.c:51`) | `reac_decode.c:26` `raw[14]\|raw[15]<<8`; `CNT_OFF 14` in `reac_ctrl.c:17` and `reac_ctrlblk.c:16` |
| one name for offset 50 | three names for it: `REAC_AUDIO_OFFSET`, `REAC_L2_HEADER_LEN` (`reac.h:22-23`) and `REAC_CTRL_BLOCK_END` (`reac_ctrlblk.h:42`); plus the local `AUDIO_OFF 50` in `reac_ctrl.c:19` and `reac_ctrlblk.c:18` |
| `reac_ctrlblk.c:15-31` macros + `put_hdr` | **DEAD** duplicate at `src/reac_ctrl.c:16-32`: nothing in `reac_ctrl.c` reads `ETH_HDR`, `CNT_OFF`, `TYPE_OFF`, `AUDIO_OFF`, `DESC_WORD_*` or `put_hdr` |
| `REAC_GRANT_GROUPB_LEN` (`reac_ctrlblk.h:859`) | redefined at `src/reac_ctrlblk.c:1317` |
| `REAC_CTRL_BLOCK_OFF + HEADAMP_REC_OFF`, `HEADAMP_REC_LEN` | `reac_ctrlblk.c:1296` `frame + 34, 6`; `:1157` `+ 16` |
| `REAC_CTRL_BLOCK_LEN` / `TYPE_OFF` | `reac_ctrlblk.c:681` `+ 32`; `:683` `frame[16]/[17]` |
| Roland OUI (no declaration exists) | `00:40:ab` at `reac_ctrl.c:43`, `reac_macaddr.c:41`, `reac_scene_body.c:86` |
| the three rates (`REAC_MODE_*` or `REAC_CFG_RATE_*`, one of the two) | literals at `reac.c:13-15,19-21,27-29`; `REAC_CFG_RATE_*` are dead (M7); the pace-code bands `8000`/`3700` at `reac.c:39-40` |
| `enum reac_role` (`reac_role.h:27`) | `REAC_CFG_ROLE_MASTER/SLAVE 0/1` (`reac_cfg.h:56-57`), and again in the vendored `REAC_CFG_ROLE_VALUE_*` |
| `REAC_CLOCK_AVAIL_*` (`reac_clock.h:87-90`) | **DEAD**: the pacer open-codes `1u << s` (`transport/src/reac_pacer.c:1459`) |
| `REAC_CLOCK_Q_COUNT` (`reac_clock.h:169`), `REAC_AUDIO_FABRIC_CEILING` (`reac_slots.h:54`) | **DEAD**: read nowhere |
| max box width 38 (spec `num_channels`) | `REAC_MAX_CHANNELS` and `REAC_BOXREG_FABRIC` (40) are used as the box-width bound (M4) |

## Discarded after re-verification (11)
1. *The classifier drops a real 0013 BOX_READY because 0013 fails the block
   checksum.* False: the captured block sums to 0 (L6 is the stale comment).
2. *`test_capture.c:220` ignores `truncate()`'s return, so the truncated-body arm is
   vacuous.* False: an untruncated file makes the arm fail, not pass.
3. *`reac_ctrl_identity_reply` payload length is off by one.* False:
   `9+sysex_len-2-20 = sysex_len-13` is exact.
4. *`ci_contains` reads past the haystack's NUL.* False: the NUL mismatches and
   stops the inner loop.
5. *The DLL can never reach UNSTABLE because only in-band samples are counted.* Not
   shown: outliers that land while tracking are counted by design
   (`reac_clock.c:170-179`).
6. *`reac_ctrl_build_grant_sweep` overruns `sweep`.* False: `max < n` is refused
   first.
7. *The scene step lengths do not sum to the declared total.* False:
   24 + 341×26 + 14 = 8904 = 0x22c8.
8. *`foreign_master` underflows when `last_seen > now`.* False: this is guarded by
   `now_ns > e->last_seen_ns`.
9. *`reac_frame_clean_len` strips a legitimate clean length.* False:
   `52+36n+2 ≡ 2 (mod 36)` is never clean.
10. *`reac_ctrl_classify_box_frame` should refuse a JOIN not addressed to us.* It is
    documented as broadcast AND unicast (the box sends it ×3 on PHY-up), per spec.
11. *`reac_headamp_sens_value_cdb` rounds a negative request toward zero.* False:
    `want <= 0` is taken first.

## Status after fix (branch `fix/libreac-review-2026-09-25`, libreac 1.6.0)

`make review-2026-09-25` is gone. Every proof that went green now runs in `make test`,
and `make test` now ends on a PASS verdict line. The sections above describe `ee205b6`
as it was reviewed.

| Finding | State | Guard in `make test` |
|---|---|---|
| M1 rate detection | fixed: one stream, measured by its own counter | `tests/test_rate_detect.c` |
| M2 clock lock on switch | fixed: a new reference is LOCKING until measured | `tests/test_clock.c` |
| M3 identity checksums | fixed: both checksums required | `tests/test_identity_cksum.c`, `tests/test_identity.c` |
| M4 box width vs desk | fixed per the operator's corrected ruling of 2026-09-25 ("BOX_MAX_CHANNELS = 40 ... S-4000S-3208 (32 in, 8 out), S-2416 (24 in, 16 out), and we tested an 8 in / 32 out box"). A box's width is even per direction, 2..40 (`reac_box_width_ok()`, bound to reac-protocol's `box_width` facts), and 40 is legal. The real bug was the rule that 40 wide means the desk. It is replaced by direction, source and role: `reac_rival_kind_of()` (arbitration), the broadcast preference in `reac_detect_rate_fd`, unicast-first in `reac_tap`, and a direction-gated `reac_rx` | `tests/test_desk_or_box.c` (32/8, 24/16, 8/32 and a 40-in box are boxes; the 40-wide broadcast announcing master is the desk; red under the width rule), `tests/test_box_width.c`, `tests/test_rx_twin.c`'s 40+40 arms (red on the old gate) |
| M4 remainder | **OPEN, operator question**: a BROADCAST audio stream whose source has sent no role-bearing control frame could be the desk's downstream or a box's presence-flood. `reac_hunt` (`box_present`, `desk_geometry_live`), `reac_tap`'s broadcast branch and `reac_segment_ident` still split it by width (`reac_rival_kind_from_channels`). Deciding it without width takes either a hold until the source's own frames resolve its role, or a time-window rule | — |
| M5 boxreg overflow | fixed | `tests/test_boxreg.c` |
| M6 plain-LE over-read | fixed | `tests/test_decode_plain_le.c` |
| M7 `reac_cfg.h` | fixed: the one declaration; the vendored reac-pw headers alias it; `"none"` | `tests/conformance-cfg-declared-once.sh`, `tests/test_cfg.c` |
| L7 fabric guard (`reac_slots.h`) | fixed: widening to 48 reds `make test` (sabotage-verified) | `tests/test_boxreg.c` |
| L1 | fixed with M7 | — |
| L2, L3 (CLOEXEC), L5, L6, L8, L9 | fixed | `test_boxreg`, `test_capture` arms |
| L3 `PACKET_IGNORE_OUTGOING` | open: this changes what every capture consumer hears; M1 no longer depends on it | — |
| L4 boxreg departure API | open: needs a new API | — |
| L7 SKIP exits 0; no tests for `reac_arbitrate` / `reac_grant` | open | — |
| D: dead `reac_ctrl.c` copy, `REAC_GRANT_GROUPB_LEN` twice, `frame + 34` | fixed | — |
| D: `0x88,0x19` byte copies, the offset-50 triple, the Roland OUI, `REAC_GRANT_SWEEP_LEN` in two headers, the dead public `REAC_CLOCK_AVAIL_*` / `REAC_CLOCK_Q_COUNT` / `REAC_AUDIO_FABRIC_CEILING` | open: spread across files, or public macros reac-pw may read | — |

**Cross-repo follow-ups.**
- reac-pw's `src/reac_rate_cfg.h` / `reac_role_cfg.h` must take the edit the vendored
  snapshot got here, and its `*_refuse_code()` must return from
  `REAC_*_REFUSE_CODES_INIT`.
- openmixer's `reac-cfg.ts` mirror must follow the `reac_cfg.h` changes: `"none"`,
  `REAC_ROLE_PROP` and `ROLE_STATE_HUNTING` added, the two unread macros removed.
