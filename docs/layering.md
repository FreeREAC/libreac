# What belongs in libreac, and what does not

Status: amended 2026-09-11 — the "Three layers" table's transport row and "The transport layer
never moves" sentence below are **superseded** by
[`docs/design/specs/2026-09-11-reac-transport-library.md`](design/specs/2026-09-11-reac-transport-library.md):
the operator ruled a second library, `libreac-transport`, takes reac-pw's socket/pacer/RT-thread
code. This note's readiness gates for the *control-plane conversation* (the establishment FSM,
§"Readiness gates for moving the FSM") are untouched — that is a different seam (protocol meaning
vs. transport plumbing) and still not ready by this note's own gates.

libreac is the one home for REAC **wire knowledge**. This note records where the
boundary runs, why the control plane is not simply "more of the same", and the
concrete conditions under which the remaining piece — the establishment FSM —
becomes safe to move here.

It exists because the boundary is not obvious. "It's REAC, so it goes in libreac"
is the wrong rule, and following it would turn a clean extraction into a rewrite
of the establishment path.

## Three layers

| layer | owns | today |
| --- | --- | --- |
| **wire format** | byte layouts, codecs, frame geometry, builders + parsers | **libreac** (builders since 0.4.0 — see Sequencing) |
| **control plane** | the REAC conversation: establishment, head-amp records, chanmap | reac-pw |
| **transport** | sockets, the SCHED_FIFO pacer, RT threads, PipeWire nodes | **reac-pw**, permanently |

The transport layer never moves. Sockets and scheduling are the consumer's
business; libreac stays IO-free and clock-free so a decoder, a bridge and an
analysis tool can all link it without dragging in a runtime.

## The control plane is two things, not one

This is the distinction the note exists to record. The control plane splits into
**vocabulary** and **conversation**, and they have opposite readiness.

### Vocabulary — pure, and ready

How to encode or parse *one* control record:

- the cold-connect records (link 4, tags `0100` / `0302` / `0500`), the box's
  declaration (link 1, opcodes `82` / `84`), the flood-filler;
- the head-amp **DT1 SysEx container** — TAG-dispatched, with **two nested
  checksums whose order is mandatory** (the inner record checksum is stamped
  first, then the outer block sum); tag `0101` is the 3-parameter head-amp page
  (phantom / pad / SENS), tag `0500` the model identity;
- the channel map.

These are pure functions over bytes. No state, no clock, no IO — **identical in
character to the audio frame builders**. They belong here for the same reason the
braid oracle does: the DT1 checksum ordering is exactly the kind of fact that must
have one home, and a second consumer is already committed to needing it —
reac-aes67 already emits REAC and is blocked on exactly this. Its encoder is
written and running (`pipewire/src/reac_tx.c`, driven by `reac_sink_node.c`);
what it lacks is the conversation — "it does **not** yet drive the connection
handshake, so a real Roland desk will not link to it" (`pipewire/src/reac_tx.h`),
with the JOIN/HOLD cold-connect sequence spelled out as the remaining work in
`pipewire/src/reac_sink_node.h`. A second implementation of the DT1 checksum
ordering is therefore not hypothetical; it is the next thing that repo has to
write unless it can call ours.

### Conversation — stateful, and not ready

The FSM: *when* to send what. The ~1.508 s ungranted hold, the ~0.494 s
grant→commit delay, the grant-sweep ordering, retry policy, the establishment
state graph.

Not a matter of effort. Two reasons it waits:

1. **It is the least-settled knowledge in the project.** An API drawn around
   facts still being revised gets churned. See the readiness gates below.
2. **It is timing-coupled, and the timings may not belong to the protocol.** The
   1.5 s hold is protocol; the SCHED_FIFO pacer is transport. Separating them
   cleanly requires the FSM to *declare* its timings as policy data rather than
   embedding them as sleeps in state handlers — a design step that must happen
   **before** extraction, not during it.

## The asymmetry that decides the order

**The vocabulary has goldens. The conversation does not.**

Every control frame that would move has real M-200 / M-300 / M-5000 bytes in
`reac-captures` to `memcmp` against, so the extraction is provable offline, byte
for byte — the same gate the audio builders passed. An FSM extraction has no
equivalent oracle: you would be proving "it still establishes", which only the rig
can answer, on a `reac-pw main` that **auto-deploys to the live rig**.

Extract what can be proven offline first. That is the whole sequencing argument.

## Readiness gates for moving the FSM

Do not start until **all** of these hold. They are written to be checkable, not
felt:

1. **The RE questions that would change the state graph are closed.** Principally
   the fabric-slot placement law (is the carrier width, a config selector, or
   config byte[9]?) and the head-amp commit/enrol semantics — both currently
   rig-gated. A state machine built over a contested transition encodes the
   contest.
2. **Timings are declared policy, not embedded sleeps.** The FSM should read its
   hold and commit intervals from a struct a caller supplies. Doing this *inside*
   reac-pw first is independently valuable and is the real precondition — it is
   what separates protocol from transport.
3. **An offline oracle exists.** A replayable establishment fixture: feed a
   captured master/box exchange to the FSM and assert the emitted sequence is
   byte-identical and the intervals fall within tolerance. Without this the move
   cannot be verified anywhere but the rig.
4. **No open defect in the establishment path.** Moving code and fixing it in the
   same step makes a regression indistinguishable from a port error.

Gate 3 is the expensive one and the most valuable regardless — it is a
regression test the project wants whether or not the FSM ever moves.

## Where it would live

A `reac_ctrl.h` module **inside libreac**, alongside `reac_braid.h` — not a new
repository. Split it out only if the session layer acquires dependencies libreac
must not have (a clock, a scheduler), which is precisely what the conversation
layer would bring and the vocabulary will not.

Do not mint a repo before there is a reason. The wrap that reac-pw already uses
covers a new header at no cost.

## Sequencing

This list is the status of the move; keep it here and nowhere else.

1. ~~**Audio frame builders** → libreac.~~ **Done in 0.4.0** —
   `<reac/reac_encode.h>` (`reac_braid_encode`, `reac_downstream_build`) and
   `src/reac_encode.c`. It went as planned: byte-identity gated against the
   capture goldens, which is what made it the safe first move and what the FSM
   still has no equivalent of.
2. **Control vocabulary + the DT1 record codec** → libreac, same gate, proven
   against the capture goldens.
3. **The FSM** → only once the four gates above hold, and only after step 2, so
   the FSM is already building its frames through libreac when it moves.

## Versioning while the control plane is out of tree

**0.5.x until `reac_ctrl` lands.** Operator decision, 2026-07-29: everything that
ships before the control plane is a **patch** bump — 0.5.1, 0.5.2, … — because
what remains in this phase is corrections and additions to the wire *vocabulary*,
not a change in what the library is.

The next **minor** (0.6.0) is reserved for the control-plane extraction described
above: when `reac_ctrl.h` appears, libreac stops being purely a layout oracle and
starts holding conversation state, which is the change a minor is for.

Recent history under this rule: 0.4.0 added the encoders (the last minor before
the freeze), 0.5.0 fixed `reac_decode()` to un-braid — a behaviour change to a
public function, so it took the minor it was already due. 0.6.0 took the minor
the control-plane extraction was reserved above.

## STANDING RULE while nothing is published (operator, 2026-08-23)

**We are not publishing yet, so a change does not owe a bump.** 0.7.0 stands, and
the next release does not need a new number merely because code changed. The
churn this stops is real: a version was moved for four consecutive changes in one
evening, which buys a bigger number and no reader — nobody outside this tree
consumes these packages, and the library and reac-pw are installed TOGETHER,
always.

**When a bump IS warranted, move the MINOR.** 0.7.0 → 0.8.0. Not a new patch
line, and not the soname.

Two things this does not relax, because they are what made a mismatched pair
detectable at all:

- **The consumer's floor moves with the version.** A number that moves while
  reac-pw's `>= …` stays put changes nothing — that inertness is the whole
  lesson of the section below.
- **Verify an upgrade by asking for what only the new build can answer**
  (`nm -D --defined-only … | grep identity_first`), never by version string,
  `dnf list`, or the daemon looking healthy.

The section below is the history that produced 0.7.0 and soname 1. Keep it as
the record of why those numbers are where they are; do not read it as a standing
instruction to move two numbers per break.

## A removed symbol moves TWO numbers

**0.7.0, and soname 0 → 1.** The rule the paragraphs above did not state, because
until then nothing had been removed: when a public function disappears, the
version digits are only half the bump. The soname is the other half, and they
fail at different moments — the version stops a BUILD against the wrong headers,
the soname stops a RUN against the wrong shared object.

This was learned the expensive way. `reac_ctrl_build_name_frame()` and
`reac_ctrl_build_extra_frame()` were removed while the library kept calling
itself 0.6.0 with soname 0, and every mechanism that should have caught it was
inert at once:

- reac-pw's `>= 0.6.0` floor accepted the old and the new library alike — meson
  reported `libreac found: YES 0.6.0` for both;
- the identical NEVRA made `rpm -U` a silent no-op;
- the dynamic linker handed the installed `/usr/bin/reac-pw` the new
  `libreac.so.0`, and it died on `undefined symbol` at exec.

Under the new rule the same change is refused three times over: the floor moves
to `>= 0.7.0` and fails at configure, the NEVRA changes so the package actually
upgrades, and `.so.1` and `.so.0` are co-installable — a stale binary keeps
loading the old one until it is replaced, instead of breaking.

**Where the two numbers live.** Both in `include/reac/reac.h` and nowhere else:
`LIBREAC_VERSION_*` and `LIBREAC_ABI`. The RPM spec carries a copy of each
(`Version:`, `%global abi`) because rpm cannot read a header, and
`packaging/make-tarball.sh` refuses to build a tarball when a copy disagrees —
the only moment a copy can be caught. The OpenWrt recipe reads both with awk and
keeps no copy at all.
