# Code reac-pw shares with libreac has one home, and it is libreac

Status: ruled by the operator, 2026-09-29 ("All the functions and code that can be shared, must be
shared"). Normative for the four places the shared-code audit of that day found the same thing
written twice across libreac and reac-pw (openmixer issues #971, #972, #980).

- **Author:** Pau Aliagas <linuxnow@gmail.com>
- **Amends:** `2026-09-17-tunables-api-and-shared-refusal-codes.md` §4 (the code list),
  `2026-09-11-reac-transport-library.md` §2/§5 (the `reac_rate_cfg.h` / `reac_role_cfg.h` seam).
- **Release order:** libreac 1.6.0 ships all of this; reac-pw raises its floor to `>= 1.6.0` in
  the same change that deletes its copies. Nothing here changes a struct layout or a soname.

## 1. The refusal/status codes: one list, two owners

`include/reac/reac_code.h` is the only `REAC_CODE_LIST`. The 2026-09-17 move left reac-pw with a
full copy "until the floor moves", and the two had diverged both ways by 2026-09-29: seven codes
existed only in reac-pw, six only here. The list is now two named halves of one X-macro:

- `REAC_CODE_LIST_DAEMON(X)` — codes a daemon linking libreac emits (reac-pw today);
- `REAC_CODE_LIST_LIBRARY(X)` — codes libreac's own sources emit;
- `REAC_CODE_LIST(X)` — both, and the only thing the enum and `reac_code_token` expand.

A code a daemon needs is added here, in its half, and ships with a libreac release. reac-pw's
`src/reac_code.h` is `#include <reac/reac_code.h>` and nothing else; its conformance script
refuses an X list of its own, and checks every DAEMON code is emitted in reac-pw and (when the
sibling checkout is present) every LIBRARY code in libreac.

## 2. The cfg vocabulary: the pure declarations move here, the snapshot goes

`reac_pacer.h` and `reac_role_swap.h` included reac-pw's `reac_rate_cfg.h` / `reac_role_cfg.h`
for pure declarations only, so this repo carried a hand-refreshed snapshot of both
(`packaging/vendor/reac-pw-headers/`) and a `REACPW_INCLUDE` build knob. The snapshot had moved
ahead of its source (the source still typed every string). The seam closes the way transport §2
said it would, by moving the pure part:

- `include/reac/reac_cfg.h` gains the rate bits (`REAC_RATE_BIT_*`, `REAC_RATE_ALL_BITS`),
  `enum reac_rate_refuse` and `enum reac_role_refuse` with their code tables
  (`REAC_RATE_REFUSE_CODES_INIT`, `REAC_ROLE_REFUSE_CODES_INIT`), and the tap's own role answer
  `REAC_CFG_ROLE_STATE_TAP` (`"role_tap"`), which reac-pw published under a local spelling.
- `reac_pacer.h` and `reac_role_swap.h` include `<reac/reac_cfg.h>`; the vendored snapshot,
  `REACPW_INCLUDE` and every `-Ipackaging/vendor/reac-pw-headers` are deleted.
- reac-pw's two headers keep only the functions reac-pw implements (the `spa_pod` parsers and
  the decisions), and its `*_refuse_code()` return from the tables.

## 3. The tap read returns the frame's own facts

reac-pw re-implemented `reac_topo_tap_next` line for line because it needs what the kernel says
about the frame — the receiving `ifindex`, whether it was our own transmission, the source MAC —
and `reac_topo_tap_next` passes no `msg_name`. `reac_topo_tap_read()` fills a
`struct reac_topo_frame` with all of them; `reac_topo_tap_next` is a wrapper over it, unchanged
for its callers.

## 4. One qdisc dump

`reac_etf_qdisc_state` and reac-pw's etf stats read each wrote the `RTM_GETQDISC` dump.
`reac_etf_qdisc_dump()` is the one walk: bounded (a fixed number of reads, a poll timeout — a
silent netlink socket cannot hold the caller), filtered to one ifindex, a callback per qdisc.
The two copies had diverged on errors: libreac read a `NLMSG_ERROR` as the end of the dump, so a
refused dump reported "no etf" instead of "unreadable"; reac-pw read the error and returned it.
The error is the right answer — `reac_etf_qdisc_state` now returns `REAC_ETF_QDISC_UNREADABLE`
for it — and reac-pw's read, which had no bound, now has the library's.
