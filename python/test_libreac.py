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
