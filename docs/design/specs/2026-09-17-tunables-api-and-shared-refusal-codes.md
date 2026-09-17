# libreac reads no environment: a tunables API replaces its own `getenv`, and refusal lines carry a code

Status: ruled by the operator, 2026-09-17 (relayed from reac-pw's
`docs/design/specs/2026-09-17-knobs-codes-and-test-ratchets.md`, §6 "Owed" — this spec closes
that debt on the libreac side). Normative for every `REACPW_*`/`REAC_*` environment variable
libreac itself reads, and for the refusal/failure lines this pass migrates.

- **Author:** Pau Aliagas <linuxnow@gmail.com>
- **Companion:** reac-pw's `2026-09-17-knobs-codes-and-test-ratchets.md` — that spec's §1 ruling
  ("a knob is discovered and PUBLISHED, never merely read") and §2 ruling ("error and status
  lines carry a stable CODE") are the law; this spec is libreac's side of conforming to both.

## 1. Why libreac cannot keep reading its own environment

A library that calls `getenv` decides behaviour the daemon that links it can neither announce
(reac-pw's `reac_knobs_announce` walks a table it owns; a value libreac read itself is invisible
to that table except as an entry marked "cannot reach, owed") nor override from `reac-pw.conf`
(the layered lookup in `reac_conf_lookup` is reac-pw's; libreac has no such lookup and reac-pw
cannot inject one into a `getenv` call already made below it). Both gaps are named as owed in
the companion spec's §6. Nine call sites read the environment directly inside libreac (`git grep
getenv src/ transport/src/` at the start of this lane, excluding `reac_envflag.h` itself — the
generic parser, not a knob site — and `reac_conf.c`, which IS the sanctioned env-reading layer
the daemon's conf precedence is built on):

`src/reac_master.c` — `REACPW_GRANT_ON_DECLARE` (via `reac_envflag`), `REACPW_GRANT_DWELL_MS`,
`REACPW_GRANT_DWELL_S`, `REACPW_NO_ENROLL`, `REACPW_EST_SCENE` (5); `transport/src/reac_pacer.c`
— `REACPW_GUARD_FLOOR_FRAMES`, `REACPW_NO_HEADAMP` (2); `transport/src/reac_ifscan.c` —
`REAC_IFACES_ALLOW_WIRELESS` (1); `transport/src/reac_rx.c` — `REAC_DEBUG` (1).

## 2. The shape — one struct per subsystem, one setter per subsystem

Argued from the existing API style (`reac_master_set_box`, `reac_master_set_headamp_src`: the
library already takes configuration through named setters, not a monolithic blob) and from the
fact these are documented as PROCESS-WIDE knobs, not per-instance state
(`reac_envflag.h`'s own header comment, unchanged by this spec): **one plain struct of tunables
per subsystem file, one setter per subsystem**, in a new public header
`include/reac/reac_tunables.h`:

- `struct reac_master_tunables` + `reac_master_tunables_set()` — the 5 `reac_master.c` knobs.
- `struct reac_pacer_tunables` + `reac_pacer_tunables_set()` — the 2 `reac_pacer.c` knobs.
- `struct reac_transport_tunables` + `reac_transport_tunables_set()` — `allow_wireless` (a
  string, matching `reac_ifscan_wireless_allowed`'s existing signature exactly — no redesign,
  the function was already pure) and `debug`.

Not one `reac_tunables_set()`: the three subsystems are already separate translation units with
no shared context object, and a single call would force every caller to know all three shapes to
set any one of them. Each struct carries `#define REAC_*_TUNABLES_DEFAULT { ... }` so a caller
that only cares about one field still gets the documented default for the rest, matching the
values the deleted `getenv` calls fell back to.

## 3. ABI

Header-only additions (a struct definition and a function declaration; the setters are ordinary
exported functions, not `static inline`, so a future body change does not need every caller
recompiled). No existing struct, symbol or signature moves. `LIBREAC_VERSION_MINOR` moves to
`3` (new public API, not a break — the rule at the top of `reac.h`: "the minor is what the
control-plane extraction takes" applies by extension to any new public surface); `LIBREAC_ABI`
stays at `4` (nothing existing changes size or offset).

## 4. Refusal lines carry a code

`include/reac/reac_code.h` moves here from reac-pw (that repo's own spec, §2, names this file's
current home as provisional: "the ideal home is libreac if that repo ever migrates them too").
Same token enum, same `reac_code_emit`, unchanged in shape — a header-only move, so reac-pw's
copy can become a thin `#include <reac/reac_code.h>` once it builds against a libreac release
that ships this file, without either repo duplicating the vocabulary in between. The six
refusal-shaped `fprintf(stderr, ...)` lines in `transport/src/` (`reac_slave.c`, `reac_rx.c`,
`reac_pacer.c` x4) migrate to `reac_code_emit` with a new token each, added to the shared list
so reac-pw's own conformance floor and this repo's mirror of it count the same vocabulary.

## 5. What this does not change

Segment/role autodetection stands, `reac_conf.c`'s own env reads (the layer-2 implementation of
the daemon's conf precedence, not a knob site itself) are untouched, and no daemon-facing
behaviour changes: every default below matches the value the deleted `getenv` call produced when
unset, so a daemon that sets nothing gets byte-identical behaviour to before this lane.
