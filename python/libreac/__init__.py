# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""ctypes bindings over the INSTALLED libreac shared object.

THIS MODULE WRAPS libreac. IT NEVER REIMPLEMENTS IT. That is the whole point and
the one rule: a Python REAC decoder written alongside the C one would be a second
layout oracle, and this project already deleted a per-width head-amp table for
exactly that reason. Every function below is a thin call into the .so — no byte
layout, no arithmetic on wire fields, no constants copied from a header.

Why ctypes and not an extension module: libreac is a stable-soname C library of
pure functions over buffers, with a pkg-config file and an RPM. ctypes needs no
build step and cannot drift out of step with the installed library, because it IS
the installed library. An extension module would need rebuilding on every bump.

Load by SONAME (`libreac.so.1`), never by the versioned filename: the soname is
the compatibility promise, the filename is an implementation detail that moves on
every release.
"""

import ctypes
import ctypes.util

_SONAME = "libreac.so.1"


def _load():
    try:
        return ctypes.CDLL(_SONAME)
    except OSError:
        found = ctypes.util.find_library("reac")
        if not found:
            raise OSError(
                f"{_SONAME} not found. Install the libreac package "
                "(libreac + libreac-devel), or set LD_LIBRARY_PATH to a build tree."
            )
        return ctypes.CDLL(found)


_lib = _load()

# The parse struct is written by the library, and its size is libreac's business,
# not ours. Rather than copy the layout here — which is precisely the second
# declaration this module exists to avoid — we hand the library a buffer far
# larger than any plausible struct and read only the RETURN value. When a caller
# needs the parsed fields, the honest fix is for libreac to export accessors, not
# for this file to learn the layout.
_PARSE_SCRATCH = 4096

_c = _lib

_c.reac_version.restype = ctypes.c_char_p
_c.reac_version.argtypes = []

_c.reac_frame_is_reac.restype = ctypes.c_int
_c.reac_frame_is_reac.argtypes = [ctypes.c_char_p, ctypes.c_size_t]

_c.reac_frame_counter.restype = ctypes.c_uint16
_c.reac_frame_counter.argtypes = [ctypes.c_char_p]

_c.reac_counter_gap.restype = ctypes.c_uint16
_c.reac_counter_gap.argtypes = [ctypes.c_uint16, ctypes.c_uint16]

_c.reac_frame_clean_len.restype = ctypes.c_size_t
_c.reac_frame_clean_len.argtypes = [ctypes.c_size_t]

_c.reac_rate_snap.restype = ctypes.c_int
_c.reac_rate_snap.argtypes = [ctypes.c_double]

_c.reac_ctrl_checksum_verify.restype = ctypes.c_int
_c.reac_ctrl_checksum_verify.argtypes = [ctypes.c_char_p]

_c.reac_ctrl_kind_name.restype = ctypes.c_char_p
_c.reac_ctrl_kind_name.argtypes = [ctypes.c_int]

_c.reac_ctrl_parse.restype = ctypes.c_int
_c.reac_ctrl_parse.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p]

_c.reac_headamp_sens_db.restype = ctypes.c_int
_c.reac_headamp_sens_db.argtypes = [ctypes.c_uint8, ctypes.c_int]

_c.reac_headamp_group_of.restype = ctypes.c_int
_c.reac_headamp_group_of.argtypes = [ctypes.c_uint8, ctypes.c_uint8]

_c.reac_ctrl_build_headamp.restype = ctypes.c_size_t
_c.reac_ctrl_build_headamp.argtypes = [
    ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
    ctypes.c_uint16, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8,
]

# The reac_ctrl builder family that needs no audio payload. Every one of these
# emits a complete control frame, so a test can build one and hand it straight to
# the recognizer, the checksum verifier and the parser — the three independent
# readers libreac already ships. The builders that take planar audio
# (upstream_filler, flood_filler, coldconnect*) are deliberately NOT bound yet:
# they need a float *const * of live samples, and a binding that fakes one would
# be testing the fake.
for _name in ("reac_ctrl_build_box_hb", "reac_ctrl_build_config_announce",
              "reac_ctrl_build_identity_first", "reac_ctrl_build_identity_last"):
    _f = getattr(_c, _name)
    _f.restype = ctypes.c_size_t
    _f.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                   ctypes.c_uint16, ctypes.c_int]

_c.reac_ctrl_box_frame_len.restype = ctypes.c_size_t
_c.reac_ctrl_box_frame_len.argtypes = [ctypes.c_int]

_c.reac_ctrl_stamp_headamp.restype = ctypes.c_int
_c.reac_ctrl_stamp_headamp.argtypes = [
    ctypes.c_char_p, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8,
]


def version() -> str:
    """The library's own version string, as the SHARED OBJECT reports it.

    This is the version of the code actually loaded, which is the only one worth
    asserting on — a package version or a header constant can disagree with what
    the dynamic linker handed us, and that disagreement is the exact failure the
    2026-08-23 soname bump exists to make impossible.
    """
    return _c.reac_version().decode()


def is_reac(frame: bytes) -> bool:
    """Does this L2 frame look like REAC (length + EtherType 0x8819)?"""
    return bool(_c.reac_frame_is_reac(frame, len(frame)))


def frame_counter(frame: bytes) -> int:
    """The u16le sequence counter at bytes 14..15. Caller must pass >= 16 bytes."""
    if len(frame) < 16:
        raise ValueError("a REAC counter lives at bytes 14..15; frame is shorter")
    return _c.reac_frame_counter(frame)


def counter_gap(last: int, cur: int) -> int:
    """Frames skipped between two counters, 16-bit wrap aware. 0 = no loss."""
    return _c.reac_counter_gap(last, cur)


def clean_len(length: int) -> int:
    """Strip the +2 FCS capture residue from a frame length, if present."""
    return _c.reac_frame_clean_len(length)


def rate_snap(pps: float) -> int:
    """Snap a measured packet rate to 44100 / 48000 / 96000."""
    return _c.reac_rate_snap(pps)


def ctrl_checksum_verify(frame: bytes) -> int:
    """0 when the control block's checksum closes. Non-zero otherwise."""
    return _c.reac_ctrl_checksum_verify(frame)


def ctrl_kind(frame: bytes) -> int:
    """Classify a control frame, returning libreac's `enum reac_ctrl_kind`."""
    scratch = ctypes.create_string_buffer(_PARSE_SCRATCH)
    return _c.reac_ctrl_parse(frame, len(frame), ctypes.cast(scratch, ctypes.c_void_p))


def ctrl_kind_name(kind: int) -> str:
    """The kind's name, for a report that has to stay diffable."""
    return _c.reac_ctrl_kind_name(kind).decode()


def classify(frame: bytes) -> str:
    """Convenience: the NAME of the control kind this frame parses as."""
    return ctrl_kind_name(ctrl_kind(frame))


def headamp_sens_db(value: int, pad_on: bool = False) -> int:
    """The dB the box applies for a raw SENS code, with or without the pad."""
    return _c.reac_headamp_sens_db(value, 1 if pad_on else 0)


def headamp_group_of(ch: int, param: int) -> int:
    """The apply group a wire channel falls in, for a head-amp parameter.

    Exposed rather than open-coded because a caller writing `ch >> k` is how the
    wrong k spreads — libreac's own header says so, and it applies here too.
    """
    return _c.reac_headamp_group_of(ch, param)


def build_headamp(master: bytes, src: bytes, counter: int,
                  ch: int, param: int, value: int) -> bytes:
    """A complete head-amp command frame, built BY LIBREAC.

    Returns the frame, or raises on a bad param/value combination — the library
    answers 0 for those and a zero-length frame is not something a caller should
    be able to put on a wire by accident.
    """
    if len(master) != 6 or len(src) != 6:
        raise ValueError("master and src are 6-byte MACs")
    out = ctypes.create_string_buffer(2048)
    n = _c.reac_ctrl_build_headamp(out, master, src, counter, ch, param, value)
    if n == 0:
        raise ValueError(
            f"libreac refused to build a head-amp frame for "
            f"ch={ch} param={param} value={value}"
        )
    return out.raw[:n]


def stamp_headamp(frame: bytes, ch: int, param: int, value: int) -> bytes:
    """Overlay a head-amp record on an already-built frame, preserving its audio.

    Returns the new frame. Raises on a bad param/value, where the library leaves
    the frame untouched and answers -1.
    """
    buf = ctypes.create_string_buffer(frame, len(frame))
    if _c.reac_ctrl_stamp_headamp(buf, ch, param, value) != 0:
        raise ValueError(
            f"libreac refused to stamp ch={ch} param={param} value={value}"
        )
    return buf.raw[:len(frame)]


def box_frame_len(n_ch: int) -> int:
    """The clean upstream frame length for a box of `n_ch` inputs."""
    return _c.reac_ctrl_box_frame_len(n_ch)


def _build_simple(fn_name: str, master: bytes, src: bytes,
                  counter: int, n_ch: int) -> bytes:
    if len(master) != 6 or len(src) != 6:
        raise ValueError("master and src are 6-byte MACs")
    out = ctypes.create_string_buffer(2048)
    n = getattr(_c, fn_name)(out, master, src, counter, n_ch)
    if n == 0:
        raise ValueError(f"libreac refused to build {fn_name} for n_ch={n_ch}")
    return out.raw[:n]


def build_box_hb(master: bytes, src: bytes, counter: int, n_ch: int) -> bytes:
    """The box heartbeat."""
    return _build_simple("reac_ctrl_build_box_hb", master, src, counter, n_ch)


def build_config_announce(master: bytes, src: bytes, counter: int, in_ch: int) -> bytes:
    """The box's config announce — the frame carrying the head-amp base strap."""
    return _build_simple("reac_ctrl_build_config_announce", master, src, counter, in_ch)


def build_identity_first(master: bytes, src: bytes, counter: int, in_ch: int) -> bytes:
    """First fragment of the identity record. BOTH fragments are one message."""
    return _build_simple("reac_ctrl_build_identity_first", master, src, counter, in_ch)


def build_identity_last(master: bytes, src: bytes, counter: int, in_ch: int) -> bytes:
    """Last fragment of the identity record. BOTH fragments are one message."""
    return _build_simple("reac_ctrl_build_identity_last", master, src, counter, in_ch)


class BoxModel(ctypes.Structure):
    """libreac's fixed box-model matrix row, mirrored field-for-field.

    Mirrored, not invented: the field order and types are libreac's
    `struct reac_box_model`. If that struct changes, this breaks loudly at the
    first read rather than returning plausible rubbish — which is why `token`
    and `display` are asserted non-empty by the tests.
    """
    _fields_ = [
        ("token", ctypes.c_char_p),
        ("display", ctypes.c_char_p),
        ("in_ch", ctypes.c_int),
        ("out_ch", ctypes.c_int),
        ("config_block", ctypes.c_uint8 * 32),
    ]


_c.reac_box_model_table.restype = ctypes.POINTER(BoxModel)
_c.reac_box_model_table.argtypes = [ctypes.POINTER(ctypes.c_size_t)]
_c.reac_box_model_by_token.restype = ctypes.POINTER(BoxModel)
_c.reac_box_model_by_token.argtypes = [ctypes.c_char_p]
_c.reac_box_model_by_channels.restype = ctypes.POINTER(BoxModel)
_c.reac_box_model_by_channels.argtypes = [ctypes.c_int]


def box_models() -> list:
    """Every box model libreac knows, looked up ONE AT A TIME by width.

    **The table is deliberately not strided.** `struct reac_box_model` carries
    the identity record after `config_block`, so this module's mirror is SHORTER
    than the real row; indexing `table()[i]` would land mid-struct and return
    plausible rubbish. Asking by width returns a pointer to a whole, correctly
    aligned row, and reading only the leading fields off it is safe.

    Widening the mirror would mean copying a layout that is libreac's to change —
    the second declaration this module exists to avoid. If the full row is ever
    needed, libreac should export accessors.

    This is the table a DESK uses to put a box's name on screen: the name is
    resolved from the box's own config-announce declaration, not read out of a
    name frame. That is why a real Roland mixer shows a name for EVERY box while
    only the S-0808 ever transmits an ASCII model name.
    """
    seen, out = set(), []
    for width in range(1, 65):
        row = box_model_by_channels(width)
        if row and row[0] not in seen:
            seen.add(row[0])
            out.append(row)
    return out


def box_model_by_channels(in_ch: int):
    """The model a box of this input width declares itself to be, or None."""
    p = _c.reac_box_model_by_channels(in_ch)
    if not p:
        return None
    return (p[0].token.decode(), p[0].display.decode(), p[0].in_ch, p[0].out_ch)
