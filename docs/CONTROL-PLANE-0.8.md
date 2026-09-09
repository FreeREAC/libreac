# `reac_link` — the control plane as a library (0.8), and whether it should be its own

**Operator ruling, 2026-09-09, verbatim:** *"move the FSM into libreac (0.8) so the daemon is
sockets + PipeWire only. This is maximum priority, reac-pw cannot do reac ctrl protocol.
Consider if we need a separate library for the connection management, separate from the audio
transmission."*

## The separate-library question — recommendation

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

## The module boundary

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

## What moves, what stays

| moves to libreac 0.8 | stays in reac-pw |
|---|---|
| `reac_fsm.{c,h}` — the slave JOIN/HOLD table | `reac_slave.c`'s I/O shell: the AF_PACKET socket, the thread, `sendto`/`recv`, the RT priority |
| `reac_master.{c,h}` + `reac_master_fsm.{c,h}` — the master establishment, grant sweep, announce/chanmap cadence | `reac_pacer.c`'s cadence: `clock_nanosleep`, the frame ring, the SCHED_FIFO thread |
| `reac_hunt.{c,h}`, `reac_arbitration.{c,h}` — which end of the pairing a wire calls for | `reac_ifscan`, `reac_linkmon`, `reac_topo` — netlink, netdevs, VLANs |
| `reac_grant.{c,h}`, `reac_headamp_tx.{c,h}` — enrolment sweep and head-amp send model | the PipeWire nodes, their props and the segment's published answer |
| every 0.5.6 rule: listen-before-speaking, no flood at an announcing master, announce after the transfer, three records, the echoed grant incl. the master's own head_mark, the descriptor after the grant, bounded retry | `main.c`'s lifecycle: listeners, conf, the 200 ms poll |
| `reac_ctrl.{c,h}`'s virtual-stagebox builders (the rest is already libreac's) | `reac_rx`, `reac_ring`, the source/sink nodes |

About **5 700 lines** of reac-pw's `src/` is control plane, and `reac_fsm.c` and
`reac_master.c` contain **zero** socket, thread or clock calls today — the boundary the
ruling asks for is already drawn, and this is mostly a move plus one API in front of it.

## The tests are the captures

`box-to-box-enroll.pcap` (S-0808 master, S-1608 joining) and its swapped-roles twin
(S-1608 master, S-0808 joining) are replayed through `reac_link` at their own timestamps, and
what it asks to send must reproduce the granted sequence **byte for byte** — the announce, the
records, the descriptor's timing, the heartbeat cadence. A rule that is not in the captures
cannot be added without a capture to justify it, and a rule that is cannot be broken without a
test going red. The veth proofs that do not need a socket become libreac tests; the ones that
prove PipeWire nodes or netdevs stay in reac-pw.
