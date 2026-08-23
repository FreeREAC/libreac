# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""Tests over the ctypes bindings — and therefore over the INSTALLED libreac.

These assert VALUES, not shapes. A binding that loads and returns something is
not evidence of anything; every case below pins a number the protocol fixes, so
a wrong library or a drifted constant fails here rather than on a stagebox.

Run: python3 -m pytest python/test_libreac.py -q   (or: python3 python/test_libreac.py)
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import libreac  # noqa: E402

MASTER = bytes.fromhex("0040ab000001")
BOX = bytes.fromhex("0040abc4803b")

PHANTOM, PAD, SENS = 0, 1, 2


def test_version_comes_from_the_shared_object():
    v = libreac.version()
    assert v.count(".") == 2, v
    # Not an equality against a hardcoded number: this asserts the loaded object
    # answers a well-formed version at all, which is what distinguishes a real
    # library from a stub. The EXACT version is pinned by the RPM and by the
    # symbol check, not here.


def test_rate_snap_hits_the_three_standard_rates():
    # pps = sample_rate / 12, so these are the only three the wire carries.
    assert libreac.rate_snap(3675) == 44100
    assert libreac.rate_snap(4000) == 48000
    assert libreac.rate_snap(8000) == 96000


def test_rate_snap_uses_midpoint_thresholds():
    # The thresholds are 3837.5 and 6000; a value either side must land apart.
    assert libreac.rate_snap(3837) == 44100
    assert libreac.rate_snap(3838) == 48000
    assert libreac.rate_snap(5999) == 48000
    assert libreac.rate_snap(6001) == 96000


def test_clean_len_strips_only_the_fcs_residue():
    # 52 + n*36 is a clean frame; +2 is the capture residue.
    assert libreac.clean_len(1494) == 1492   # 40 ch downstream
    assert libreac.clean_len(1206) == 1204   # 32 ch S-4000 return
    assert libreac.clean_len(342) == 340     # 8 ch S-0808 return
    # Every clean length is returned unchanged — the rule must not "fix" them.
    assert libreac.clean_len(1492) == 1492
    assert libreac.clean_len(1204) == 1204
    assert libreac.clean_len(340) == 340


def test_counter_gap_is_wrap_aware():
    assert libreac.counter_gap(10, 11) == 0        # consecutive: no loss
    assert libreac.counter_gap(10, 13) == 2        # two skipped
    assert libreac.counter_gap(0xFFFF, 0) == 0     # wrap, consecutive
    # 0xFFFE -> 1 crosses 0xFFFF and 0x0000, so TWO frames are missing, not one.
    assert libreac.counter_gap(0xFFFE, 1) == 2


def test_all_three_headamp_params_address_a_single_channel():
    """The per-channel actuation law, pinned on the wire helper.

    Settled 2026-08-23 on the wire (2304 of 3651 records address a channel that
    is not a multiple of four) AND at the hardware (writing phantom to a group
    neighbour left a live condenser ~45 dB above the floor). So the addressing
    granularity is ONE CHANNEL for phantom, pad and sens alike, and this test
    exists to keep any future "group of four/eight" reading from creeping back.

    Note the 8-row APPLY unit is a DIFFERENT axis: the box applies in groups of
    eight, but a record still addresses one channel. Do not conflate them.
    """
    for param in (PHANTOM, PAD, SENS):
        for ch in (0, 1, 7, 8, 0x20, 0x2F):
            assert libreac.headamp_group_of(ch, param) == ch, (ch, param)


def test_sens_curve_is_one_flat_db_per_step_and_DESCENDS():
    """Uniform 1 dB per step, and the curve goes DOWN as the code goes up.

    sens_db(0) = -10 and sens_db(55) = -65: a 55 dB span in 55 steps, sloping
    negative. The direction is asserted explicitly because getting it backwards
    is silent — every "one flat dB" property still holds on a curve that rises,
    and the error only shows up as a preamp driven the wrong way.
    """
    assert libreac.headamp_sens_db(0) == -10
    assert libreac.headamp_sens_db(55) == -65
    for v in range(0, 55):
        step = libreac.headamp_sens_db(v + 1) - libreac.headamp_sens_db(v)
        assert step == -1, (v, step)


def test_built_headamp_frame_is_reac_and_its_checksum_closes():
    # The round trip that matters: libreac builds it, and libreac's INDEPENDENT
    # recognizer and checksum verifier both accept it. A frame we emit that our
    # own reader rejects is the defect this catches.
    frame = libreac.build_headamp(MASTER, BOX, 0x1234, 0x2F, PHANTOM, 1)
    assert libreac.is_reac(frame), "libreac built a frame its own recognizer rejects"
    assert libreac.ctrl_checksum_verify(frame) == 0, "checksum does not close"
    assert libreac.frame_counter(frame) == 0x1234


def test_built_frame_classifies_as_a_control_kind_with_a_name():
    frame = libreac.build_headamp(MASTER, BOX, 1, 0x20, SENS, 32)
    name = libreac.classify(frame)
    assert isinstance(name, str) and name, name
    # A frame we built must not classify as the unknown/none kind.
    assert "unknown" not in name.lower(), name


def test_build_refuses_an_out_of_range_value():
    # The refusal is the contract: libreac answers 0 and the binding raises
    # rather than handing back a zero-length frame someone could transmit.
    try:
        libreac.build_headamp(MASTER, BOX, 1, 0x20, SENS, 250)
    except ValueError:
        return
    raise AssertionError("expected libreac to refuse SENS=250")


def test_stamp_preserves_length_and_keeps_the_checksum_closed():
    frame = libreac.build_headamp(MASTER, BOX, 7, 0x20, PHANTOM, 0)
    stamped = libreac.stamp_headamp(frame, 0x21, PHANTOM, 1)
    assert len(stamped) == len(frame)
    assert stamped != frame, "stamping a different channel changed nothing"
    assert libreac.ctrl_checksum_verify(stamped) == 0


def test_a_non_reac_buffer_is_rejected():
    # The negative control. Without it, is_reac() returning True for everything
    # would pass every test above.
    assert not libreac.is_reac(b"\x00" * 64)



# --- the reac_ctrl builder family, each frame checked by THREE readers ---------
#
# For every control frame libreac can build: its own recognizer accepts it, its
# own checksum closes, its own parser names a kind, and — when the grammar is
# reachable — reac.ksy accepts it too. The grammar is the INDEPENDENT opinion:
# libreac agreeing with itself proves only internal consistency.

CTRL_BUILDERS = [
    ("box_hb", lambda c, n: libreac.build_box_hb(MASTER, BOX, c, n)),
    ("config_announce", lambda c, n: libreac.build_config_announce(MASTER, BOX, c, n)),
]

WIDTHS = [8, 16, 32]


def test_every_ctrl_builder_produces_a_frame_its_own_readers_accept():
    built = 0
    for name, fn in CTRL_BUILDERS:
        for width in WIDTHS:
            frame = fn(0x0042, width)
            built += 1
            assert libreac.is_reac(frame), f"{name}/{width}: recognizer rejects it"
            assert libreac.ctrl_checksum_verify(frame) == 0, f"{name}/{width}: checksum"
            assert libreac.frame_counter(frame) == 0x0042, f"{name}/{width}: counter"
            kind = libreac.classify(frame)
            assert kind and "unknown" not in kind.lower(), f"{name}/{width}: {kind}"
    # A sweep that built nothing would pass every assertion above.
    assert built == len(CTRL_BUILDERS) * len(WIDTHS), built


def test_ctrl_frame_length_matches_the_box_width_formula():
    # 52 + n*36, and libreac's own helper must agree with the frames it builds.
    for width in WIDTHS:
        frame = libreac.build_box_hb(MASTER, BOX, 1, width)
        assert len(frame) == 52 + width * 36, (width, len(frame))
        assert len(frame) == libreac.box_frame_len(width), width


def test_the_grammar_accepts_every_frame_libreac_builds():
    """The independent second opinion on our own TX.

    SKIPS when the grammar is unreachable, and says so — an absent grammar is
    not a clean grammar, and reporting it as a pass is the exact failure this
    project keeps paying for.
    """
    from libreac import ksy

    if not ksy.available():
        print(f"     SKIP: {ksy.reason_unavailable()}")
        return

    checked = 0
    for name, fn in CTRL_BUILDERS:
        for width in WIDTHS:
            frame = fn(9, width)
            assert ksy.validates(frame), f"grammar rejects our own {name}/{width}"
            checked += 1
    for ch in (0x00, 0x20, 0x2F):
        frame = libreac.build_headamp(MASTER, BOX, 3, ch, PHANTOM, 1)
        assert ksy.validates(frame), f"grammar rejects our own headamp ch={ch:#x}"
        checked += 1
    assert checked > 0
    # The negative control: the grammar must REJECT something, or "accepts
    # everything" would look identical to "accepts our frames".
    assert not ksy.validates(b"\x00" * 64), "grammar accepts garbage"


def test_identity_record_is_built_only_for_the_box_that_carries_a_name():
    # The S-0808 (8 in) transmits an ASCII model name; the wider boxes do not,
    # and libreac refuses to build one for them. This is NOT the same as a desk
    # being unable to NAME them — see the box-model table test below.
    frame = libreac.build_identity_first(MASTER, BOX, 5, 8)
    assert libreac.is_reac(frame) and libreac.ctrl_checksum_verify(frame) == 0
    for width in (16, 32):
        try:
            libreac.build_identity_first(MASTER, BOX, 5, width)
        except ValueError:
            continue
        raise AssertionError(f"expected no identity record for a {width}-input box")


def test_every_known_box_resolves_to_a_display_name():
    """How a desk names a box it never received a name frame from.

    A real Roland mixer shows a name for EVERY box. That name is resolved from
    the box's own declared width via this fixed matrix — not read off the wire —
    which is why only the S-0808 needs to transmit one.
    """
    models = libreac.box_models()
    assert len(models) >= 3, models
    tokens = {t for t, _, _, _ in models}
    assert {"s0808", "s1608", "s4000s"} <= tokens, tokens
    for token, display, in_ch, out_ch in models:
        assert token and display, (token, display)
        assert in_ch > 0 and out_ch > 0, (token, in_ch, out_ch)
        # The width round-trips: the matrix is keyed on what the box declares.
        assert libreac.box_model_by_channels(in_ch)[0] == token


def test_matrix_names_EVERY_width_an_s1608_which_is_a_DEFECT():
    """CHARACTERIZATION TEST — this pins a bug, not a contract.

    `reac_box_model_by_channels()` never answers "I do not know": widths 1, 7, 9,
    24, 40 and 64 all come back as an S-1608. It is a fallback wearing a lookup's
    clothes.

    That matters for the stated use — verifying the values a box puts on the wire
    against the matrix. A verifier that answers for every input cannot fail, so it
    cannot verify: a corrupt announce, or any box model we have not met, is
    confidently reported as an S-1608 and nothing downstream can tell.

    It is also why `box_models()` must enumerate by trying known widths rather
    than trusting this function to reject the rest.

    WHEN LIBREAC IS FIXED to return NULL for an unmatched width, THIS TEST WILL
    FAIL. That is the intent: replace it with the negative control it should have
    been —
        assert libreac.box_model_by_channels(7) is None
    """
    for width in (1, 7, 9, 24, 40, 64):
        got = libreac.box_model_by_channels(width)
        assert got is not None and got[0] == "s1608", (width, got)


if __name__ == "__main__":
    fns = [(n, f) for n, f in sorted(globals().items()) if n.startswith("test_")]
    failed = 0
    for name, fn in fns:
        try:
            fn()
            print(f"ok   {name}")
        except Exception as exc:  # noqa: BLE001
            failed += 1
            print(f"FAIL {name}: {exc}")
    print(f"\n{len(fns) - failed} passed, {failed} failed")
    sys.exit(1 if failed else 0)
