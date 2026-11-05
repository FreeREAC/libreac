#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""Compare two replays of the capture corpus (tools/peer_replay.c, one run per build) and
refuse any difference that is not explained.

    peer-replay-compare.py BASE_DIR FIXED_DIR --expected tests/peer-replay-expected.txt
    peer-replay-compare.py BASE_DIR FIXED_DIR --write    tests/peer-replay-expected.txt

A directory holds one sub-directory per capture and, in it, one record file per arm
(rx-*.bin, master-*.bin, hunt.bin) plus meta.bin, one record per pcap record, so record i
of every file is frame i. Layouts are in tools/peer_replay.c.

EXPLAINED means this script derived the fixed build's answer from the FRAME and the
design's own rule, without asking the library. Each arm has one rule:

  rx      counter-not-ahead   A frame the base decodes is refused when its counter is not
                              ahead of the last frame the fixed build accepted, unless
                              REAC_RX_RESYNC_RUN (8) consecutive refused frames continue
                              each other. This is replayed over the whole file and the
                              result must equal the fixed build's record for EVERY frame,
                              not only the ones that differ: an outcome, or the digest of
                              the samples a decoded frame wrote, that moves without the
                              rule is unexplained.
  master  peer-binding        A frame the base accepted is refused when the master held a
                              box (GRANTING or ESTABLISHED, MAC not zero), the frame's
                              source is not that box, and it is not a JOIN while GRANTING.
                              The state and event are the fixed build's own, read before
                              the frame; the source is the frame's. The refusal is the ONLY
                              thing allowed to differ in a record.
  hunt    rival-not-streaming The verdict differs only where the rival the base named had
                              not yet streamed: fewer than REAC_HUNT_RIVAL_FRAMES (8)
                              sightings, or a span under REAC_HUNT_RIVAL_SPAN_NS (20 ms), or
                              not heard for REAC_HUNT_RIVAL_QUIET_NS (100 ms), counted from
                              the sightings in the capture as of the hunt's last step
                              (an entry unheard for REAC_DISCO_STALE_NS, 5 s, is gone and
                              starts again).

Any other difference is printed with its frame and exits 1. The expected file records, per
capture and stream, how many frames differ, the first one, and the rule; a difference that
is new, gone or moved is also red, even when the rule explains it.
"""
import os
import struct
import sys

RESYNC_RUN = 8
RIVAL_FRAMES = 8
RIVAL_SPAN_NS = 20 * 1000 * 1000
RIVAL_QUIET_NS = 100 * 1000 * 1000
HUNT_POLL_NS = 200 * 1000 * 1000
STALE_NS = 5 * 1000 * 1000 * 1000      # REAC_DISCO_STALE_NS: an entry unheard this long is gone

M_GRANTING, M_ESTABLISHED = 2, 3
EV_JOIN = 1
NO_RIVAL, ZERO_MAC = 0, bytes(6)

MAGIC = {"rx": b"PRRX0001", "master": b"PRMA0002", "hunt": b"PRHU0002", "meta": b"PRME0001"}
RECLEN = {"rx": 8, "master": 24, "hunt": 32, "meta": 32}


def records(path, arm):
    data = open(path, "rb").read()
    if data[:8] != MAGIC[arm]:
        sys.exit("peer-replay-compare: %s: not a %s record file" % (path, arm))
    n = RECLEN[arm]
    body = data[8:]
    if len(body) % n:
        sys.exit("peer-replay-compare: %s: a record is cut short" % path)
    return [body[i:i + n] for i in range(0, len(body), n)]


def arm_of(name):
    for a in ("rx", "master", "hunt"):
        if name.startswith(a):
            return a
    return None


def counter_ahead(last, cur):
    return ((cur - last - 1) & 0xFFFF) < 0x8000


class Report:
    def __init__(self):
        self.lines = {}          # (capture, stream) -> expected-file line
        self.unexplained = []
        self.frames = 0
        self.compared = 0
        self.differ = {"rx": 0, "master": 0, "hunt": 0}
        self.streams = {"rx": 0, "master": 0, "hunt": 0}

    def bad(self, capture, stream, i, why):
        self.unexplained.append("%s %s frame %d: %s" % (capture, stream, i, why))


def check_rx(rep, cap, stream, a, b):
    """Replay the counter rule over the base's outcomes and require the fixed build's
    record, whole, for every frame."""
    have = False
    last = run = slast = 0
    differ = 0
    first = None
    resyncs = longest = streak = 0     # followed restarts; most frames refused in a row
    for i, (x, y) in enumerate(zip(a, b)):
        pred = x
        if x[0] in (1, 4):                       # decoded or bad: reached the counter
            ctr = x[1] | x[2] << 8
            refused = False
            if have and not counter_ahead(last, ctr):
                run = run + 1 if (run and ctr == ((slast + 1) & 0xFFFF)) else 1
                slast = ctr
                if run < RESYNC_RUN:
                    pred = bytes([2]) + x[1:4] + bytes(4)
                    refused = True
                else:
                    have = False
                    resyncs += 1
            if refused:
                streak += 1
                longest = max(longest, streak)
            else:
                streak = 0
            if not refused:
                run = 0
                have = True
                last = ctr
        if pred != y:
            rep.bad(cap, stream, i, "fixed %s, the counter rule says %s (base %s)"
                    % (y.hex(), pred.hex(), x.hex()))
        if x != y:
            differ += 1
            if first is None:
                first = i
    # resyncs: a restarted counter that was followed after REAC_RX_RESYNC_RUN frames.
    # longest: the most frames refused in a row; 8 or more with no resync means the
    # refused frames do not continue each other (a capture cut to every Nth frame).
    return differ, first, "counter-not-ahead resyncs=%d longest-refused=%d" % (resyncs, longest)


def check_master(rep, cap, stream, a, b, meta):
    differ = 0
    first = None
    for i, (x, y) in enumerate(zip(a, b)):
        if x == y:
            continue
        differ += 1
        if first is None:
            first = i
        src = meta[i][8:14]
        st_before, ev, box = y[0], y[1], y[2:8]
        held = st_before in (M_GRANTING, M_ESTABLISHED) and box != ZERO_MAC
        refuses = held and src != box and not (ev == EV_JOIN and st_before == M_GRANTING)
        # the refusal is the whole difference: the same record but for the accepted flag
        same_but_acc = x[:9] == y[:9] and x[10:] == y[10:] and x[9] == 1 and y[9] == 0
        if not (refuses and same_but_acc):
            rep.bad(cap, stream, i, "base %s fixed %s: source %s, master held box %s in state %d, "
                    "event %d: not a refusal by the binding" % (x.hex(), y.hex(), src.hex(),
                                                              box.hex(), st_before, ev))
    return differ, first, "peer-binding"


def check_hunt(rep, cap, stream, a, b, meta):
    """Walk the sightings the way the tool does (a step every 200 ms of capture time, before
    the frame), keep the evidence as of the last step, and require every differing verdict to
    be one whose rival had not streamed by then."""
    differ = 0
    first = None
    per_variant = [0, 0, 0]      # frames whose verdict differs: unpinned, pinned master, pinned slave
    seen = {}                    # mac -> [sightings, first_ns, last_ns], live
    snap = {}                    # the same, as of the last step
    snap_t = 0
    next_step = None
    for i, (x, y) in enumerate(zip(a, b)):
        ts = struct.unpack_from("<Q", meta[i], 0)[0] * 1000
        if next_step is None:
            next_step = ts
        if next_step <= ts:
            # the steps due before this frame; ageing is monotone in the step time, so the
            # last one decides which entries are gone (reac_disco_table_age)
            last_step = next_step + ((ts - next_step) // HUNT_POLL_NS) * HUNT_POLL_NS
            for k in [k for k, v in seen.items()
                      if last_step > v[2] and last_step - v[2] > STALE_NS]:
                del seen[k]
            snap = {k: list(v) for k, v in seen.items()}
            snap_t = last_step
            next_step = last_step + HUNT_POLL_NS
        if x[24]:
            src = meta[i][8:14]
            e = seen.get(src)
            if e is None:
                seen[src] = [1, ts, ts]
            else:
                e[0] += 1
                e[2] = ts
        if x == y:
            continue
        diffs = [v for v in range(3) if x[v * 8:v * 8 + 8] != y[v * 8:v * 8 + 8]]
        if not diffs:
            rep.bad(cap, stream, i, "records differ outside the three verdicts")
            continue
        differ += 1
        if first is None:
            first = i
        for v in diffs:
            per_variant[v] += 1
            bx, by = x[v * 8:v * 8 + 8], y[v * 8:v * 8 + 8]
            mac = bx[2:8]
            e = snap.get(mac)
            streams = (e is not None and e[0] >= RIVAL_FRAMES and
                       e[2] - e[1] >= RIVAL_SPAN_NS)
            quiet = e is None or snap_t - e[2] > RIVAL_QUIET_NS
            why = None
            if bx[1] == NO_RIVAL or mac == ZERO_MAC:
                why = "the base named no rival"
            elif streams and not quiet:
                why = ("rival %s had streamed (%d sightings over %.1f ms, last %.1f ms before "
                       "the step)" % (mac.hex(), e[0], (e[2] - e[1]) / 1e6, (snap_t - e[2]) / 1e6))
            elif bx[1] != by[1] or bx[2:8] != by[2:8]:
                why = "the rival itself differs"
            if why:
                rep.bad(cap, stream, i, "variant %d base %s fixed %s: %s"
                        % (v, bx.hex(), by.hex(), why))
    return differ, first, "rival-not-streaming unpinned=%d pinned-master=%d pinned-slave=%d" % tuple(per_variant)


def main():
    args = sys.argv[1:]
    if len(args) != 4 or args[2] not in ("--expected", "--write"):
        sys.exit(__doc__)
    base, fixed, mode, expected = args
    rep = Report()
    got = []
    caps = sorted(os.listdir(base))
    if sorted(os.listdir(fixed)) != caps:
        sys.exit("peer-replay-compare: the two runs hold different captures")
    for cap in caps:
        names = sorted(f for f in os.listdir(os.path.join(base, cap)) if f.endswith(".bin"))
        if names != sorted(f for f in os.listdir(os.path.join(fixed, cap)) if f.endswith(".bin")):
            rep.bad(cap, "-", 0, "the two runs wrote different record files")
            continue
        meta = records(os.path.join(base, cap, "meta.bin"), "meta")
        rep.frames += len(meta)
        for name in names:
            arm = arm_of(name)
            if arm is None:
                continue
            pa, pb = os.path.join(base, cap, name), os.path.join(fixed, cap, name)
            rep.streams[arm] += 1
            if os.path.getsize(pa) != os.path.getsize(pb):
                rep.bad(cap, name, 0, "the record files differ in length")
                continue
            n = (os.path.getsize(pa) - 8) // RECLEN[arm]
            if n != len(meta):
                rep.bad(cap, name, 0, "%d records for %d frames" % (n, len(meta)))
                continue
            rep.compared += n
            if open(pa, "rb").read() == open(pb, "rb").read():
                continue
            a, b = records(pa, arm), records(pb, arm)
            if arm == "rx":
                d, first, rule = check_rx(rep, cap, name, a, b)
            elif arm == "master":
                d, first, rule = check_master(rep, cap, name, a, b, meta)
            else:
                d, first, rule = check_hunt(rep, cap, name, a, b, meta)
            rep.differ[arm] += d
            if d:
                got.append("%s %s frames=%d differ=%d first=%d rule=%s"
                           % (cap, name[:-4], n, d, first, rule))
    got.sort()

    print("captures %d, frames %d, records compared %d (rx %d, master %d, hunt %d streams)"
          % (len(caps), rep.frames, rep.compared, rep.streams["rx"], rep.streams["master"],
             rep.streams["hunt"]))
    total = sum(rep.differ.values())
    print("differences %d records (rx %d, master %d, hunt %d) in %d streams; unexplained %d"
          % (total, rep.differ["rx"], rep.differ["master"], rep.differ["hunt"], len(got),
             len(rep.unexplained)))

    if mode == "--write":
        if rep.unexplained:
            for u in rep.unexplained[:20]:
                print("UNEXPLAINED", u)
            print("peer-replay-compare: not writing an expected file over unexplained differences")
            return 1
        with open(expected, "w") as f:
            f.write("# Differences between the base and the fixed libreac across the capture corpus,\n"
                    "# each explained by the rule named (tools/peer-replay-compare.py). Empty of\n"
                    "# data lines means the two builds agree on every frame. Regenerate with\n"
                    "# tools/peer-replay.sh --write-expected, and read the diff.\n"
                    "# capture stream frames differ first-frame rule [detail]\n")
            for g in got:
                f.write(g + "\n")
        print("wrote %s (%d lines)" % (expected, len(got)))
        return 0

    want = [l.rstrip("\n") for l in open(expected) if l.strip() and not l.startswith("#")]
    rc = 0
    for u in rep.unexplained[:40]:
        print("UNEXPLAINED", u)
    if rep.unexplained:
        rc = 1
    for line in sorted(set(got) - set(want)):
        print("NEW OR MOVED  ", line)
        rc = 1
    for line in sorted(set(want) - set(got)):
        print("GONE OR MOVED ", line)
        rc = 1
    print("FAIL" if rc else "OK: every difference is explained and matches %s" % os.path.basename(expected))
    return rc


if __name__ == "__main__":
    sys.exit(main())
