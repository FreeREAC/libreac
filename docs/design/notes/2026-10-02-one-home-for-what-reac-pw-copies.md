<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# One home for what reac-pw copies: the code list, the cfg vocabulary, the tap read, the qdisc dump

Shared-code audit lane A (FreeMixer/openmixer #971, #972, #980). libreac and the ksy own the
protocol; reac-pw is a consumer. Four things reac-pw carries a copy of belong here, in the
libreac-devel / libreac-transport-devel public headers, and this note says what each home is,
which copy was right where they differ, and what refuses a second copy afterwards. reac-pw
moving onto them is its own lane, after this release is out and baked.

## What is copied, and where it lives now

| what | reac-pw's copy | libreac's home |
|---|---|---|
| refusal/status code X-macro | `src/reac_code.h`, its own `REAC_CODE_LIST` | `<reac/reac_code.h>` |
| `reac.cfg.*` / `reac.rate.*` / `reac.role` vocabulary | `src/reac_rate_cfg.h`, `src/reac_role_cfg.h` spell every string | `<reac/reac_cfg.h>` |
| topology tap read (frame + sender) | `main.c` `topo_tap_read()`, its own `recvmsg` + auxdata parse | `<reac/transport/reac_topo.h>` |
| etf qdisc counters (RTM_GETQDISC dump) | `src/reac_qdisc.c` `reac_qdisc_stats_read()` | `<reac/transport/reac_etf_qdisc.h>` |

### The code list

The two lists share their first nine tokens and then diverge: libreac's has its transport's
own (`E_PROMISC_FAILED`, `E_CAPTURE_FAILED`, `E_QDISC_READ_FAILED`, `E_ETF_REFUSED`,
`S_KNOB_IGNORED`, `S_HEADAMP_SUPPRESSED`), and reac-pw's has seven libreac never took
(`E_LINK_BUDGET`, `E_ORPHAN_PAIR`, `E_ROSTER_REMOVE`, `E_ROSTER_NODE`, `E_UNKNOWN_KNOB`,
`S_BUDGET_YIELDED`, `S_NO_OVERRIDES`). No token is spelled two ways, so neither copy is
wrong. The home becomes the union. The seven are appended so every existing enumerator
keeps its value, and a test pins the invariant the list relies on: every token is its
enumerator's name without `RC_`, and every token is unique.

### The cfg vocabulary: which copy is right

The vendored snapshot (`packaging/vendor/reac-pw-headers/`) names `<reac/reac_cfg.h>`'s
macros, and reac-pw's own headers still type the strings. Every string reac-pw types equals
libreac's value. That includes the idle refusal, `"none"`, which libreac moved to in 1.6.0.
So the values have not drifted, and libreac's declaration is the right copy for every key
it carries.

One answer is missing from it. reac-pw's `reac_role_cfg.h` declares the tap's own role state,
`REAC_ROLE_STATE_TAP "role_tap"`, published on `reac.cfg.role.state` by a segment whose intent
is `tap`. libreac's `reac_cfg.h` has no such member, and the vendored snapshot dropped it, so
the snapshot is behind its source on this one line. The tap is libreac's own declaration:
`REAC_ROLE_INTENT_TAP` in `<reac/reac_role.h>`, `<reac/transport/reac_tap.h>`. The ksy has
nothing to say about PipeWire props, so the protocol's owner on this question is libreac. A
role state it does not declare is a value a console cannot name from the one declaration.
The fix is `REAC_CFG_ROLE_STATE_TAP "role_tap"` in `reac_cfg.h`, with the snapshot aliasing
it as reac-pw's header does. It lands in its own commit, red first in `tests/test_cfg.c`.

### The tap read

`reac_topo_tap_next()` returns the kind and the VID and drops what the kernel reports about
the sender. reac-pw needs that sender information for a regression it already met on the
rig: a frame from another interface queued before the bind. So reac-pw re-implemented the
whole read (`recvmsg`, the PACKET_AUXDATA walk, the TP_STATUS_VLAN_VALID rule) to get
`sll_ifindex`, `sll_pkttype` and the source MAC. The home adds `struct reac_topo_frame`
(kind, vid, source MAC, ifindex, outgoing) and `reac_topo_tap_read()`. `reac_topo_tap_next()`
becomes a wrapper over it, so the socket read exists once. The namespace test
(`tests/test_topo_hears_vlans.c`) proves the new fields against a real veth: the ifindex is
the tapped parent's, and the source MAC is the far end's.

### The qdisc dump

libreac's `reac_etf_qdisc_state()` and reac-pw's `reac_qdisc_stats_read()` are two
RTM_GETQDISC dumps over the same socket shape. The home is one bounded dump walker in
`reac_etf_qdisc.c`, and both doors run over it:

- `reac_etf_qdisc_state()`: unchanged answers, including UNREADABLE for a dump that
  produced nothing.
- `reac_etf_qdisc_stats_read()`: new, with `struct reac_etf_qdisc_stats`. It sums the
  counters of every etf qdisc on the device. TCA_STATS2 is read first, with TCA_STATS as
  the fallback. Unreadable returns -errno and is never a zero. A dump that does not reach
  NLMSG_DONE is an error, because a partial sum would under-report drops.

The namespace test installs etf on a veth through the library's own door and sends unstamped
frames, which sch_etf with skip_sock_check must drop. It then requires the drops to be
counted, nothing launched, and `qdiscs` to read 0 before the install and after the removal.

## The gate

`tests/conformance-cfg-declared-once.sh` was already the "declared once" ratchet for the cfg
vocabulary. It is extended rather than joined by a parallel gate, and renamed
`tests/conformance-declared-once.sh` because it now covers more than cfg. New arms:

- **ARM 4**: `REAC_CODE_LIST` is defined only in `include/reac/reac_code.h`, and none of its
  tokens is typed as a string literal anywhere else.
- **ARM 5**: an RTM_GETQDISC request is built only in `transport/src/reac_etf_qdisc.c`.
- **ARM 6**: the tap's auxdata tag rule (`& TP_STATUS_VLAN_VALID`) is applied only in
  `transport/src/reac_topo.c`.

Each arm carries a planted good/bad pair, like the existing ones. With no arguments the gate
scans libreac's own tree, which is what `make test` runs. Directories given as arguments are
scanned as consumers, so the reac-pw lane can point the same gate at its `src/` and watch it
go from red to green. Run against reac-pw's `src/` today, it names `reac_code.h`'s list and
its 16 tokens, `reac_qdisc.c`'s dump, `main.c`'s tap read, and every cfg string its two
headers type. ARM 2 is unchanged and compares values, so it also lists
`reac_headamp_state.h`'s `"applied"`. That is the same word on a different prop, and the
reac-pw lane decides between aliasing it and calling it a convention, as `"none"` already is.

## The version

`include/reac/reac.h` is the one place the version lives. It already reads 1.6.0, and 1.6.0
is not tagged (the last tag is v1.5.0), so these additions join 1.6.0's notes there and in
both spec changelogs instead of moving the number. Every addition is a new symbol, struct or
macro, and no existing struct or symbol moves, so LIBREAC_ABI stays 4. The two new structs
enter `tests/abi-layout.inc` through `tools/gen-abi-layout.py`.
