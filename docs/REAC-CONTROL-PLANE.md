# The REAC control plane, and `reac_link`

**Status: current.** This is how a REAC endpoint pairs with another one, what libreac does
about it since 0.8.0, and which parts are measured rather than assumed. Everything below was
taken from real gear on 2026-09-09 unless it says otherwise; the two captures it rests on are
a real S-1608 enrolling with a real S-0808 in master mode, and a real S-0808 enrolling with a
real S-1608 in master mode, both granted.

## 1. The pairing, end to end

A REAC segment has one master and one or more boxes. Which end a daemon takes is decided by
what is already on the wire (`<reac/reac_hunt.h>`); a stagebox with its Mode switch on M
masters its own segment and is joined as a slave, exactly as a desk is.

**The joining side, in the order the wire shows:**

| step | what | evidence |
|---|---|---|
| listen | say nothing for about two announce cadences | both granted boxes were silent 15 s and 4 s first |
| flood | ONLY at a master that does not announce itself: bounded broadcast FILLER | the box joining the silent S-0808 flooded 5459 frames / 0.68 s; the box joining the announcing S-1608 broadcast **nothing** |
| wait | until the master's scene transfer has been quiet ~200 ms | joins landed +0.411 s and +0.217 s after the last scene record; `reac.ksy`: a box joining mid-transfer must not cancel it |
| announce | unicast `cdea 0103 0010`, declaring YOUR OWN inventory at your width | S-0808 `01 01 01 01 02 02`, S-1608 `02 02 02 02 01 01`, both selector 0x80, each granted by the other |
| burst | ~200 ms later: `cdea 0403` TAG 0100 JOIN, then TAG 0302 BOX_READY | `16.1162`, `16.1164` |
| heartbeat | on the VERY NEXT frame, before any grant | `16.1165`, two milliseconds before the grant |
| grant | the master answers: echo(JOIN) + **its own** TAG 0000 head_mark + echo(BOX_READY) | the S-0808 sent two records and was answered with three |
| steady | unicast at the wire rate + `cdea 0103 0001 81` about once a second | 1.004 s measured |

**The FILLERS carry a state while all that happens**, in their control area `[18:50]`, sixteen
times `00 xx`:

| descriptor | when | measured |
|---|---|---|
| zero | before the announce | 48 frames |
| `0x52` requesting | announce → grant | 8691 frames, exactly the announce-to-burst gap |
| `0x7a` established | after the grant | the rest |

A replay of the granted capture with that middle window zeroed is **refused** by a real
S-1608 — the only variant of it that is. The S-0808 as master tolerates zeros, which is why a
daemon that never sent `0x52` enrolled with one box and not the other.

**A box on M runs no courtship of its own beyond announcing:** the S-0808 emits no `cfea` at
all, the S-1608 emits one about once a second plus a repeating scene transfer. Both grant.

## 2. What is NOT settled

- **The data words of the three DT1 records** (`06 00 01 00`, `03 00 00 00`, `00 01 00`) are
  captured bytes, not read fields. See the appendix.
- **The slot count** a slave sends is 8 in both captures, which is the master's output count
  AND the slave's own — every box here is 8-out, so the two readings are not told apart.
- **Why a real S-1608 in master mode still refuses reac-pw** while granting a replay of a real
  box's frames at any phase. Every field has been cleared by replay; the outstanding candidate
  is the missing heartbeat above, which is implemented and awaiting a rig run.

## 3. `reac_link` — the API



`reac_link` is PURE: no sockets, no timers, no threads, no PipeWire. It is fed and it answers.

```c
struct reac_link;                       /* opaque; caller allocates via reac_link_size() */

enum reac_link_role { REAC_LINK_MASTER, REAC_LINK_SLAVE_TO_DESK, REAC_LINK_SLAVE_TO_BOX };

struct reac_link_cfg {
    enum reac_link_role role;
    int      fps;                       /* the wire's slot rate; the caller measures it */
    uint8_t  src[6];                    /* our L2 source                                */
    int      declared_ch;               /* what we announce ourselves as                */
    int      wire_ch;                   /* the peer's declared width, when it has one    */
    unsigned flags;                     /* frame geometry, experiment knobs             */
};

/* ONE ENTRY POINT PER EVENT, and the same answer shape from both. */
struct reac_link_out {
    const uint8_t *frame;  size_t len;  /* what to send, or len == 0                    */
    uint8_t  dst[6];       int is_bcast;/* where — the address is part of the decision   */
    unsigned events;                    /* REAC_LINK_EV_* bits: state change, grant,
                                         * box identified, drop, width known             */
    enum reac_link_state state;         /* the whole state, always readable              */
};

void reac_link_init(struct reac_link *l, const struct reac_link_cfg *cfg);
void reac_link_rx  (struct reac_link *l, const uint8_t *frame, size_t len,
                    uint64_t now_ns, struct reac_link_out *out);
void reac_link_tick(struct reac_link *l, uint64_t now_ns, struct reac_link_out *out);
void reac_link_phy (struct reac_link *l, int up, uint64_t now_ns, struct reac_link_out *out);

/* What the caller may ask about, rather than infer: the peer's identity and geometry, the
 * pace reference, the refusal code — the facts reac-pw publishes on its nodes. */
const struct reac_link_peer *reac_link_peer(const struct reac_link *l);
```

`now_ns` is passed IN. The library never reads a clock, so a capture can be replayed through
it at its own timestamps and the answer is deterministic — which is what makes the tests
below possible at all.


## 4. One library, two headers



**One library, two headers, and split only when a control-plane-only consumer exists.** The
reasons are specific rather than tidy-minded:

- **They share the frame.** Every control decision is carried in the same 0x8819 frame the
  audio rides in: the block at `[16:50]` and the braid at `[50:1490]` are two regions of one
  buffer, and both sides call `reac_ctrl_*` to read or stamp them. A split would put the
  frame's layout in one library and its meaning in another, and the layout is the thing that
  must not fork.
- **They share the model matrix.** Widths, port tables, head-amp straps and the box identity
  are read by the encoder (how many slots to fill) and by the enrolment (what to declare and
  what to grant). One table, one place.
- **Nothing today would link only one.** reac-pw needs both. The only consumers that would
  want control alone are a monitor or a test harness, and both are better served by a header
  boundary than by a second `.so` with its own version, soname and drift check — this
  repository has already paid for one version drift this week.
- **A header boundary gives most of the benefit now.** `<reac/reac_link.h>` can be complete
  and self-contained, with no `reac_encode.h` in its API, so a future split is a build change
  and not a redesign. That is the cheap option that keeps the expensive one open.

**When to revisit:** the moment something links libreac for control and never encodes a frame
— a bridge, a protocol analyser, an embedded box emulator. Then the split has a consumer to
answer to, and the header boundary drawn now is where it cuts.


The operator decided for one library, 2026-09-09.

## Appendix — what the firmware does and does not say

 — UNKNOWN at firmware level, and here is why

**No literal 0x0100 / 0x0302 / 0x0000 tag comparison or the data words `06 00 01 00`,
`03 00 00 00`, `00 01 00` were found anywhere in any of the three decompiled images
searched** (`S-1608_alldecomp.c` 44,604 lines, `S-4000_alldecomp.c` 43,444 lines,
`M-400_alldecomp.c` — grepped for the hex byte sequences, for `0x302`/`0x0403` as
16-bit immediates, and for a `switch`/`case` dispatch on any of these tag values: zero
hits in all three). This is not a silent/broken search — it is a **known, already
catalogued gap**, not something newly discovered here:

- `reac-firmware-re/FIRMWARE-INDEX.md:88` (Open RE target #2): *"Recover the literal
  op 0403 parser that writes the staging table `DAT_0c003e14` (also data-section
  dispatched) — confirms the head-amp record → staging binding."* — i.e. the S-1608's
  RX-side link-4/op-0403 record dispatcher has never been located; it hangs off a
  data-section pointer table Ghidra's recursive descent never resolved into named
  functions, the same class of gap as the per-slot ENROLLED-bit writer
  (`FIRMWARE-INDEX.md` table `0c01e604` row: *"Writer dispatches through a
  data-section pointer table NOT in the function-only export"*).
- The TX side (the box's own *emission* of the three join-burst records) is in the
  same boat: no S-1608 function builds a 16-bit tag compare or holds these six data
  bytes as a literal array anywhere the grep tools above can see.
- `libreac/src/reac_ctrlblk.c:936-953` (`reac_ctrl_build_coldconnect_head`, added for
  0.7.2, **today**) says the same thing about its own record, in its own words
  (`include/reac/reac_ctrlblk.h:467-486`): *"WHAT THE DATA MEANS is not settled by one
  capture and is not claimed here."* The header explicitly credits only the
  **arithmetic** — the Roland DT1 rule `sum(tag..cksum) mod 256 == 0x80` reproduces
  0x78/0x7d/0x7a for the three records — not a firmware source for the data words.

**So the honest answer to "are 06 00 01 00 / 03 00 00 00 / 00 01 00 constants, state
fields, or table entries" is UNKNOWN.** `join_grant_data` in `reac.ksy:2093-2097` names
tag 0x0100's shape as `06 00 XX 00` with XX "the box's join state, climbing 0x01 -> 0x09
over the establishment" but says the driver of the climb "is UNRESOLVED — it is not
modelled here." No decompiled function was found that writes any of the three
records' data bytes, so nothing here upgrades that to a firmware fact.

**What IS firmware-evidenced nearby, for context:** the record immediately BEFORE
this burst on the wire — the config-announce / commit report (op 0103, page 0x10) —
**is** recovered, in full, at `S-1608_alldecomp.c:5791` (`FUN_0c003c8a`, the scene
FSM's state-4 COMMIT). See §4.

## 2. Master-side handling — wire-evidenced, not firmware-evidenced

No firmware is available for the master role in today's specific pairing: the
S-0808 (used here as the REAC master) has an **empty decompile** — 1 line, "not
decompiled" — per `FIRMWARE-INDEX.md:12`. The closest available master firmware,
M-400, has its `CMasterReacManager`'s state-machine logic (which would include
whatever gates an echo) sitting behind **9 virtual functions absent from the
export** because they are reached only through vtable dispatch that Ghidra's
recursive descent never walked — `devices/M-400/notes.md`: *"All nine
CMasterReacManager virtual functions are absent from the export while the code IS
in the image... Re-running with forced entry points at the vtable slots should
surface them."* So the master's accept/echo decision has not been located in any
decompile checked here, for any device.

What IS established, from the wire (today's own capture, already written into
`libreac/include/reac/reac_ctrlblk.h:479-484`, EVIDENCE section):

> *a real S-1608 in slave mode enrolling with a real S-0808 in master mode,
> 2026-09-09, `box-to-box-enroll.pcap` t=6.833: the three records go out back to
> back and the master echoes ONE `cdea 04 03` per DISTINCT record — three for
> three. A burst that repeats a record draws two echoes, measured by replaying
> the same capture with its second record replaced by a copy of the first.*

That is a positive-and-negative control in one measurement (repeat the burst with
a duplicate substituted, and the echo count changes accordingly), so it is solid:
**the master echoes each DISTINCT record independently — it is not gated on
having seen all three, and no one record (e.g. `0x0302`) is treated as special.**
It matches the general rule already on record for the whole `cdea 04 03` family
(`reac.ksy` doc, and `reac_ctrlblk.h:459-461`): *"The master echoes the control
block verbatim as its grant... it learns the box from the L2 source, so no
inventory tail is needed."* No check on the announce's family selector (0x80/0x82)
or on a record count was found or evidenced — the echo appears to be a pure
byte-content relay, keyed on the record's own bytes.

**The corollary the task's context already states is therefore explained, not
resolved further here:** if a slave FSM's "grant" condition requires *distinct*
echoes of all three tags (0x0100, 0x0000, 0x0302) before it will call itself
enrolled, sending record 1 twice and getting two echoes back can never satisfy
that condition — the master did exactly what it always does (echo what it was
sent), and the burst simply never contained the other two tags. No firmware
evidence was found for what the SLAVE's own echo-counting logic requires (the
S-1608's link-1 scene FSM, `FUN_0c0037ee` / states 2-4, governs the *scene
transfer* commit, not this link-4 exchange, and no link-4-specific FSM function
was located — same gap as §1).

## 3. Does `reac.ksy` already define this block? Yes — fully

`spec/reac.ksy` already has a complete grammar for the container and for two of
the three tags:

- `container_0403` (line 1530): dispatches op 0x0403's payload on `payload[0]`
  (0x00 → `dt1_record`, 0x02 → the box's unrelated upstream `box_return_block`
  look-alike).
- `dt1_record` (line 1793): the Roland SysEx envelope — wrapper `00 02 00 fe`,
  `F0 41 0a`, model id `00 00 12`, command (0x11 RQ1 / 0x12 DT1), then
  `tag: u2, enum: reg_page`, then `data` switched on tag, then the inner
  checksum and `F7`.
- `reg_page` enum (line 2109-2114): `0x0000: head_mark`, `0x0100: join_grant`,
  `0x0101: head_amp`, `0x0302: box_ready`, `0x0500: identity`.
- `dt1_record.data`'s switch (line 1851-1860) only names **three** of the five
  tags a struct: `head_amp_data`, `join_grant_data`, `identity_data`. **`head_mark`
  (0x0000) and `box_ready` (0x0302) have no dedicated payload struct** — their
  data bytes parse as an unnamed 4- or 3-byte blob, not a decoded record. This is
  the spec-level mirror of §1: the tags are named, their data is not.
- `join_grant_data` (line 2093): *"TAG 0x0100 — the join grant. Observed as
  `06 00 XX 00` with XX the box's join state, climbing 0x01 -> 0x09 over the
  establishment... what advances it is UNRESOLVED — it is not modelled here."*
  Today's rec1 (`06 00 01 00`) is XX=0x01, consistent with this being read early
  in the climb.

No `head_mark_data` / `box_ready_data` struct exists to quote beyond the enum
name — that is the honest state of the spec, not an omission in this note.

## 4. The config-announce: selector byte and port-type table

**Selector.** `reac.ksy`'s `commit_report_page.selector` doc (line 1383) is
firmware-EVIDENCED, and it fully explains the byte the task measured:

> *"FUN_0c003c8a picks it at run time between two literal-pool bytes on a
> link-state test — S-1608 0x82 (DAT_0c00401c) or 0x80 (DAT_0c00401e)... Observed
> 0x82 on the S-1608... a box in the second arm would report a byte this schema
> has never seen paired with its width."*

Confirmed directly in the decompile, `S-1608_alldecomp.c` (`FUN_0c003c8a`, state-4
COMMIT, starts line 5791):

```
5857   sVar3 = (*(code *)PTR_FUN_0c0040e4)();      // an unnamed, data-dispatched
5858   if (sVar3 == 1) {                            // link-state test
5863       puVar1[4] = (char)DAT_0c00401c;          // selector = 0x82 arm
...
5877   } else {
5880       puVar1[4] = (char)DAT_0c00401e;          // selector = 0x80 arm
5881       puVar1[5] = 0; puVar1[6] = 0; puVar1[7] = 0;   // board_config_code FORCED 0
```

**Today's captured selector is 0x80** — this is the "second arm" the spec's own
doc says "this schema has never seen paired with [the S-1608's] width." That is a
genuinely new corpus data point: the first observed instance of the 0x80 branch
for a 16-input box, produced by whatever `PTR_FUN_0c0040e4()` tests (link state —
not identified further here; it is itself a data-section indirect call, same
class of gap as §1). One firmware consequence worth flagging: per lines 5880-5881
the 0x80 arm hard-codes `board_config_code` (the byte right after the two
reserved `00 00` bytes) to **0x00**, whereas the corpus's 0x82-arm captures show
0x02 there (`libreac/src/reac_ctrlblk.c:398-401`, `BOX_MODELS[s1608].config_block`).
The task's own hex for this frame is given abbreviated ("…"), so this note does
not attempt to re-derive that byte position from it — check the raw capture
against this prediction (0x00, not 0x02) before relying on it.

**Port-type table.** The 8+ bytes quoted in the task (`02 02 02 02 01 01 03 03…`)
are `commit_report_page.cells` (`reac.ksy:1409`, 12 bytes, `enum: inventory_cell`
— line 2119: `0x01 output`, `0x02 analog_input`, `0x03 absent`). They line up
exactly with the byte-verified S-1608 table already in libreac
(`src/reac_ctrlblk.c:399-402`, `config_block` bytes 8-19): four `analog_input`
cells (4×4 = 16 in), two `output` cells (2×4 = 8 out), six `absent` — i.e.
"4 + 2" cells, matching `reac.ksy`'s stated S-1608 declaration exactly
(`declared_in_channels`/`declared_out_channels` instances, line ~1420).

## Open items (unresolved, stated as such)

1. What `06 00 01 00` (JOIN), `03 00 00 00` (HEAD_MARK) and `00 01 00` (BOX_READY)
   mean in firmware terms — no S-1608/S-4000S/M-400 function was found that reads
   or writes these bytes. Depends on recovering the op-0403 data-section dispatch
   (`FIRMWARE-INDEX.md` open RE target #2).
2. What the S-0808 (or any master) checks before echoing, beyond "echo the bytes
   it was sent" — no master firmware is available for this pairing (S-0808 image
   empty; M-400's master vtable functions unrecovered).
3. **len_echo discrepancy, worth re-checking against the raw capture.** Two of
   today's three records (rec1, rec2 — both `rec_len 0x14`) carry `len_echo = 0x0e`.
   Every previously byte-verified capture of the same envelope
   (`libreac/src/reac_ctrlblk.c` `BOX_MODELS[*].cc0014`, `GRANT_HEAD_ACK`,
   `GRANT_HEAD_MARK` — all `rec_len 0x14`) carries `len_echo = 0x0f`, matching the
   documented rule `len_echo == rec_len - 5` (`reac.ksy:602`, confirmed against the
   `0x13`-length records: rec3 here and `cc0013` both agree at `len_echo = 0x0e`).
   Both deviations land on the same byte in the same two records rather than
   scattering, which argues against a one-off transcription slip, but this note
   cannot tell a genuine firmware/box-revision difference from a transcription
   artifact in the hex handed to it — re-read the two bytes directly from
   `box-to-box-enroll.pcap` before changing any table on the strength of this.
4. What drives the join-state field XX in `06 00 XX 00` (`reac.ksy`'s own
   "UNRESOLVED"), and what the slave's own grant-completion gate requires
   (distinct echoes of all three tags, evidenced only by today's daemon behaviour,
   not by any recovered slave-side link-4 FSM function).

## Master-mode grant path (S-1608 firmware) — 2026-09-09

New rig fact: an S-1608 rebooted into MASTER mode never echoes the enrolment
burst; the same burst, sent to an S-0808 in master mode, is granted. Searched
`S-1608_alldecomp.c` (44,604 lines, 1,314 recovered functions) for the four
questions asked. Result: **none of the master-mode machinery was found**, and
there is a documented reason to distrust the one place earlier notes claimed it
lived.

**(1) M/S switch.** No hit for `master`/`slave`/`role`/`strap`/`dipsw` anywhere
in the file — but a positive control shows this null result is worthless on its
own: `grep` for the file's own known ASCII content (`2.200`, `ECM42`, `S-0808`,
`S-1608`) also returns **zero** hits. This decompile carries **no string
literals at all**; version/model strings cited elsewhere in this repo were read
from the raw `.BIN`, not this export. The one boot-time GPIO strap that IS
documented, `0x0c080918` (`FUN_0c00f738`, GPIO port 1 bit 2), is independently
settled as a **board/model identity strap** ("16in/8out vs 16out/8in cell
layout"), not a REAC role — `BOOT-STATE-S1608-2026-08-23.md` lines 18-56 — and
reads `2` on every real S-1608 in the corpus. No other GPIO-read call site
exists in the file (`FUN_0c01f020`, the GPIO reader, has exactly one caller).
**No M/S switch code was located.**

**(2) Master RX/echo path for 0x0403.** Not found, and not findable by the
method used: `FIRMWARE-INDEX.md:88` (Open RE target #2, already on record)
says the op-0403 record parser "writes the staging table `DAT_0c003e14` (also
data-section dispatched)" — i.e. it hangs off an unresolved indirect-call
table, the same gap as this file's earlier section. No code that copies an
RX'd control block into a TX buffer was found anywhere in the file (grepped
for the `memcpy`-style loops used elsewhere, e.g. `FUN_0c003c8a`'s copy at
lines 5822-5834, which copies the box's own staging→active table, not a peer
frame). **No same-model/OUI/selector check exists to find, because no
dispatcher was found to hang it on.**

**(3) Own probe/announce cycle.** Not found. `grep` for the wire type words
(`0xcdea`/`0xcfea`, both byte orders) returns **zero** hits — expected, not a
gap: `FIRMWARE-INDEX.md`'s own search-trap note says the type word, like the
ethertype and end marker, is FPGA-applied and appears in no firmware image.
So absence of the literal proves nothing either way. What IS checkable —
whether the CPU builds probe/sub01/sub02/cfea-shaped 32-byte blocks the way it
builds the box's own chanmap/heartbeat blocks — turned up no such builder:
`FIRMWARE-INDEX.md`'s S-1608 function table lists only RX ingest
(`FUN_0c002e94`→`FUN_0c002d42`) and two upstream (box→master) TX builders
(`FUN_0c002bb2`, `FUN_0c002c70`), no master-style downstream builder. **No
probe/announce cycle was located in this image.**

**(4) Same-family refusal.** No such check was found, because no master-mode
dispatcher was found at all (see (2)).

**A documented contradiction, resolved in the negative direction.**
`NATIVE-REAC-DESIGN.md:105` and `analysis/REAC-PROTOCOL-FROM-SOURCE.md:511-521`
(§1.9) both once described this same address range (`0x0c00xxxx-0x0c04xxxx`)
as **"an M-300 master" codebase embedded in the S-1608 image**, and named
`FUN_0c0037ee` "`master_link_fsm`" (idle→wait-peer→dispatch→probe→commit→
lc-arm→lc-test) with `FUN_0c003c8a` as its "`commit_build_established`" master
announce-builder. `analysis/REAC-PROTOCOL-FROM-SOURCE.md:521-527` **retracts
this explicitly, dated 2026-08-23**: *"`FUN_0c003c8a` IS THE BOX'S, NOT THE
MASTER'S... it is dispatched from state 4 of the S-1608's own scene FSM
`FUN_0c0037ee`... a design that has the master running this to 'become MASTER'
has the roles backwards."* Reading `FUN_0c0037ee` directly
(`S-1608_alldecomp.c:5414-5511`) confirms the correction: it is one flat
8-state loop on a single global (`*DAT_0c003c08`→`*DAT_0c003964`) with **no
role branch anywhere in it** — states 2/3/4/7 call exactly the box-role
functions `FIRMWARE-INDEX.md` already attributes to the box's scene/link-check
FSM (`FUN_0c003aae`, `FUN_0c003b88`, `FUN_0c003c8a`, `FUN_0c004026`). Since
state 4 is confirmed box-role, the whole FSM it is one state of cannot also be
the master's. **The place earlier notes pointed to for "the S-1608 becoming
master" does not hold that logic; wherever the master-mode path actually lives
in this image, it has not yet been identified.**

**Net:** this cannot distinguish "the S-1608 firmware has a real same-model
refusal" from "the S-1608's master mode never implements the grant-echo at
all" (a capability gap, not a rejection) — both predict exactly the observed
symptom (never echoes), and no code was found to decide between them.

## Mixer-side parsing (M-5000 dumps, M-400 decompile) — 2026-09-09

**(1) M-5000 dumps: negative, well-controlled.** None of `m5000_addrwrite_raw.txt`,
`m5000_acs.txt`, `m5000_43caller_raw.txt` contain 0x0100/0x0000/0x0302/0x0101 or
the data bytes — but so does every other `.txt` in the directory: grepping for
the already-known, byte-verified 0x0101 head-amp tag across **all 60+ M-5000
dump files** returns **zero** hits anywhere, so this is a real absence, not a
narrow miss. The three named files are C++ object plumbing (destructors, a
generic SysEx `F0..F7` wrapper at `m5000_f0f7_raw.txt:64-79`/`FUN_00043ad8`) and
a 5-entry table lookup keyed by a 16-bit value (`m5000_43caller_raw.txt:51-84`,
`FUN_00043c08`, "7bitMaskBody") that is *shaped* like a 5-tag dispatch (reg_page
has exactly 5 members) but whose table contents (`DAT_00043c50`) are not printed
in any dump — **cannot be confirmed as the reg_page table.**

**(2)+(3) M-400: FOUND — the announce/declaration ingest, not the 0x0403 one.**
`FUN_8c0ce400` (`M-400_alldecomp.c:109549-109613`) sits inside the address range
`devices/M-400/notes.md` already flags as the master state machine's
neighbourhood. It checks `block[0]==1 && block[1]==3` (op 0x0103, single-frame),
then switches on **the exact same selector byte** this note's §4 covers
(`block[4]`, offset `param_1+8`): arms for `0x80`, `DAT_8c0ce570` (an
unconfirmed literal — presumably `0x82`, not printed in this export), `0x83`
(`-0x7d` signed) and `0x84` (`-0x7c` signed) all funnel into one call,
`PTR_FUN_8c0ce580(frame, selector, board_config_code@block[7], &cells@block[8],
12, family_flag)` — the args landing exactly on `commit_report_page`'s
`selector`/`board_config_code`/`cells[12]` layout. **The 0x83 and 0x84 arms
make one extra call the 0x80/0x82 arms never make** (`PTR_FUN_8c0ce588(frame, 0
or 2)`, lines 109596/109601) — matching, independently, libreac's
wire-inferred `has_identity_record` split (S-0808/S-4000S need the identity
name fragment, S-1608 doesn't). The 0x80/0x82 arms alone also accept a
**short-form** declaration (`rec_len==3`, line 109569) that skips the cell
ingest entirely and still returns accepted.

**No same-model/peer-identity check exists in this function** — all four
selectors are parsed identically regardless of who is speaking, so a "same
model as me" refusal, if real, is not here. `PTR_FUN_8c0ce580/584/588` are
themselves indirect calls with no visible body in this export (same vtable-only
gap as `notes.md`'s nine missing `CMasterReacManager` methods), so whatever
slot-allocation/admission decision follows the parse is still opaque. **No
switch on tag 0x0100/0x0000/0x0302 was found anywhere near this function or
elsewhere in the file** — this is the op-0103 announce parser, not the op-0403
DT1 dispatcher, so it does not explain the measured master-emits-its-own-
`head_mark` behaviour either. `CReacUnitListDT1Msg`/`CLibDT1Msg` (named in
`notes.md` from RTTI, not from this export) return zero hits in the plain-text
decompile — unreachable by the same method used here.

## Work-tree images (S-0808, S-1608 gaps, M-200i) — 2026-09-09

**(1) S-0808 — premise does not hold; still no usable decompile anywhere
checked.** `reac-firmware-re-work/S-0808-decomp/S-0808_alldecomp.c` is **71
bytes, one line** — byte-identical placeholder to the `devices/` copy, not a
fuller export. `proj-0808/p.rep/idata/00/~00000000.db/db.1.gbf` (the Ghidra
project behind it) is **980 KB with zero `FUN_` symbols** — loaded, never
auto-analyzed. So none of the master-mode questions (M/S strap, 0x0403
RX/echo, the master's own `head_mark` emission, peer/MAC checks, FLOOD
handling) can be answered from firmware text — there is none to search, in
either location.

**(2) S-1608 gap.c/gap2.c — real content, wrong neighbourhood.** Both are
genuine gap-fill exports (92 and 93 newly-named functions) from
`proj-1608`/`proj-1608b` (6.4 MB, 1,486 `FUN_` symbols each — a real superset
of the 1,314 in `devices/S-1608_alldecomp.c`). Zero hits for `f0`/`41`/`0a`,
`00 00 12`, any of the three tags, or a 16-bit switch. Checked
programmatically: **every recovered function address in both files falls
outside `0x0c002800-0x0c004600`**, the scene-FSM/chanmap/op-0403
neighbourhood — they instead fill the head-amp hardware-apply cluster
(beside the already-known `FUN_0c007fbc`/`FUN_0c007e30`), a GPIO-**write**
pair (`FUN_0c01f0d0`/`FUN_0c01f0e4`, not another strap **read**), and an
uncharacterized ~30-function cluster at `0x0c016e-0181`. **Neither gap fill
touches the 0x0403 dispatcher or a master-mode state machine.**

**(3) M-200i — the project is real and much larger, but unexported.**
`proj-m200i-sh2a/p.rep/idata/00/~00000000.db/db.4.gbf` is a **98 MB** live
Ghidra project against `m200i_sys_v1054/rsff/MPRG.sh2.bin` (string-confirmed)
carrying **8,747** auto-named `FUN_` symbols — genuinely much fuller than
`devices/M-200i`'s 272-function stub, so the premise holds here. But it is a
binary Ghidra database, not a text export: no `.c`/`.txt` file exists in this
work-tree to grep, and `strings` on the database finds only
Ghidra-autogenerated `FUN_0cXXXXXX` labels — no analyst-added name
(`master`/`join`/`grant`/`dt1`) anywhere, meaning it has never been
semantically annotated. **Cannot confirm or refute a 0x0403 handler here —
the searchable artefact does not exist yet.** Producing one needs a headless
decompile pass (`scripts/ForceDecomp.java` is present for exactly this) run
against `p.gpr`, which is new analysis work, not this read-only pass.

## S-1608b neighbourhood and M-200i (2026-09-09 exports) — the dispatcher is still absent, and there's a reason to stop looking for it in the CPU

**Positive controls.** `S-1608b_alldecomp.c`: `FUN_0c003c8a` present, byte-identical body
(3 hits). `M-200i-sh2a_alldecomp.c`: 8,475 functions (matches the recipe log) and
`PTR_s_REAC_master_is_running_on__skHz_...` / `PTR_s_time_out_wait_REACRDY_...` strings
present — this export, unlike the S-1608/S-4000S/M-400 ones, carries strings.

**(1) S-1608b — dispatcher absent; the 175-function sweep covered 0x0c002516–0x0c016de0**,
but only **19 are genuinely new** vs the old `S-1608_alldecomp.c` (diffed by address), and
all 19 sit at `0x0c0082d0-0x0c008742` (head-amp-adjacent), not the link-4 region. Zero hits
for the DT1 envelope bytes, the three tags, or a 16-bit switch, in either new file. **What
IS newly readable there: a second, previously-unrecovered MASTER-role link FSM**
(`FUN_0c002fee`/`FUN_0c003000`, near-duplicate 8-state loops, `S-1608b_alldecomp.c:4368-4507`)
that calls `FUN_0c003398` (the scene-push builder) at state 2 and a **new function**,
`FUN_0c00350a` (`:5230-5261`), at state 3. `FUN_0c00350a` reads a frame and requires
`block[0]==1 && block[1]==3` (op 0x0103, single-frame) **and** `block[4]` equal to one of
**two** data-section constants (`DAT_0c003672`/`DAT_0c003674`) **and** `rec_len∈{3,0x10}`
**and** `block[5]==0` — then, on `rec_len==0x10`, ingests `board_config_code` and the
12-cell table exactly like `commit_report_page`. **This answers "does a master need the
peer's announce first" — yes, structurally required, and it is the SAME shape as M-400's
`FUN_8c0ce400`.** It still never touches link 4/tags 0x0100/0x0000/0x0302.

**(2) M-200i — same announce-parser shape, and it settles an M-400 unknown.**
`FUN_0c0fd4ec` (`M-200i-sh2a_alldecomp.c:147901-147996`) is byte-for-byte the same
logic: op 0x0103/single-frame, `block[4]` (as signed `cVar1`) tested against `-0x80`
(`0x80`) or **`-0x7e` (`0x82`)** for one arm (`rec_len∈{3,0x10}`), `-0x7d` (`0x83`) and
`-0x7c` (`0x84`) for two more, each funnelling into one ingest call with a family flag —
0x83/0x84 each make one extra call the 0x80/0x82 arm doesn't, same as M-400. **This
confirms, for the first time, that M-400's unread literal `DAT_8c0ce570` is `0x82`** — three
independent implementations (M-400, M-200i, and the S-1608 acting as master) now agree on
the identical four-way selector dispatch. No op-0403/tag switch was found near it or via
the same search (`grep`s for the envelope bytes, the tags, and a `case 0x100`/`0x302`
style switch: all empty across 268,085 lines).

**Why to stop expecting a CPU dispatcher.** This is now four images (S-1608, S-1608b,
M-400, M-200i) and the M-5000 dumps all agreeing on the same absence in the same
shape — op-0103 announce parsing is real and recovered everywhere it's been looked for,
op-0403 tag dispatch is nowhere. That matches an **already-settled fact in this repo**,
not a new one: `analysis/REAC-PROTOCOL-FROM-SOURCE.md`'s reconciliation table (§9, "The
`cdea 04 03` grant bytes" row) already concluded *"Not built in CPU code... FPGA frames
the grant; CPU reaches state-4 commit and hands a parsed object... chasing grant bytes in
CPU is a dead end."* Four more images agreeing with that is corroboration, not a new
finding — the join-burst echo this whole note chases may simply not be CPU-resident on
any of these desks, on the same architecture that applies the ethertype, end marker and
block checksum in hardware.
