# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""Validate frames against the KAITAI GRAMMAR — a second, independent reader.

WHY THIS EXISTS. libreac and reac-protocol's `reac.ksy` are two independent
expressions of the same wire. They already agree frame-for-frame over 6.9M
CAPTURED control frames — but that only exercises the RECEIVE side. What nothing
checked is the frames WE EMIT: reac-pw's own TX was verified only by hand-written
byte fixtures in C, which is how a test can go on asserting a sequence the
protocol has outgrown.

Pointing the grammar at our own output closes that gap. A frame libreac builds
that the grammar rejects is a frame no real desk would have built, and we find it
here instead of on a stagebox.

THE GRAMMAR IS NOT VENDORED. It is located at runtime in the reac-protocol
checkout, exactly as libreac's Makefile locates `protocol-facts.yaml` — same
`REAC_PROTOCOL` convention, same reason: a copied grammar is a second grammar,
and it drifts. When the checkout is absent, `available()` answers False and the
caller SKIPS rather than silently passing. A validator that cannot find its
grammar must never look like a validator that found nothing wrong.
"""

import os
import sys
import functools

_ENV = "REAC_PROTOCOL"
_DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "..", "..", "reac-protocol")


def spec_dir() -> str:
    """Where the grammar's generated parsers live, or '' when not resolvable."""
    root = os.environ.get(_ENV) or _DEFAULT
    d = os.path.abspath(os.path.join(root, "spec"))
    return d if os.path.isfile(os.path.join(d, "reac.py")) else ""


@functools.lru_cache(maxsize=1)
def _parser():
    d = spec_dir()
    if not d:
        return None
    try:
        import kaitaistruct  # noqa: F401
    except ImportError:
        return None
    if d not in sys.path:
        sys.path.insert(0, d)
    try:
        import reac as _reac
        from kaitaistruct import KaitaiStream, BytesIO
    except ImportError:
        return None
    return (_reac, KaitaiStream, BytesIO)


def available() -> bool:
    """True when the grammar can actually be applied.

    Callers SKIP on False. They must not treat it as a pass — see the module
    docstring: an absent grammar and a clean grammar are not the same answer.
    """
    return _parser() is not None


def reason_unavailable() -> str:
    """A sentence saying why validation cannot run, for a skip message."""
    if available():
        return ""
    if not spec_dir():
        root = os.environ.get(_ENV) or _DEFAULT
        return (f"reac-protocol grammar not found under {root!r}; "
                f"set {_ENV} to a checkout")
    return "kaitaistruct is not installed (pip install kaitaistruct)"


def parse(frame: bytes):
    """Parse one frame with the grammar, returning the Kaitai object.

    Raises whatever the grammar raises — the exception IS the finding, and it
    names the field that did not fit.
    """
    p = _parser()
    if p is None:
        raise RuntimeError(reason_unavailable())
    reac, KaitaiStream, BytesIO = p
    return reac.Reac(KaitaiStream(BytesIO(frame)))


def validates(frame: bytes) -> bool:
    """Does the grammar accept this frame? Never raises."""
    try:
        parse(frame)
        return True
    except Exception:  # noqa: BLE001 — any grammar rejection counts
        return False


def check_pcap(path: str, limit: int | None = None) -> dict:
    """Run the grammar over every REAC frame in a pcap.

    Returns {'frames', 'ok', 'failed', 'first_error'}. **`frames` is part of the
    result on purpose**: a run that parsed nothing reports `frames: 0` rather
    than `failed: 0`, so a caller can refuse to read an empty scan as a pass.
    That distinction is the one this project keeps paying for.
    """
    from . import is_reac  # the recognizer, not a length heuristic

    out = {"frames": 0, "ok": 0, "failed": 0, "first_error": None}
    for frame in _pcap_frames(path):
        if not is_reac(frame):
            continue
        out["frames"] += 1
        try:
            parse(frame)
            out["ok"] += 1
        except Exception as exc:  # noqa: BLE001
            out["failed"] += 1
            if out["first_error"] is None:
                out["first_error"] = f"frame {out['frames']}: {exc}"
        if limit is not None and out["frames"] >= limit:
            break
    return out


def _pcap_frames(path: str):
    """Yield raw L2 frames from a classic pcap. Little- and big-endian."""
    import struct

    with open(path, "rb") as fh:
        hdr = fh.read(24)
        if len(hdr) < 24:
            return
        magic = hdr[:4]
        if magic == b"\xd4\xc3\xb2\xa1":
            end = "<"
        elif magic == b"\xa1\xb2\xc3\xd4":
            end = ">"
        else:
            raise ValueError(f"{path}: not a classic pcap (magic {magic.hex()})")
        while True:
            rec = fh.read(16)
            if len(rec) < 16:
                return
            _, _, caplen, _ = struct.unpack(end + "IIII", rec)
            data = fh.read(caplen)
            if len(data) < caplen:
                return
            yield data
