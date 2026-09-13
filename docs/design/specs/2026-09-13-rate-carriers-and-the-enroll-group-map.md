# The rate carriers are one value; the ENROLL group map is the desk's

Status: measured 2026-09-13 and implemented this lane (`src/reac_master.c`, `src/reac.c`,
`tests/test_master_carriers.c`). Supersedes two standing claims in this repo: that the
cfea/ENROLL/chanmap byte is a CONSOLE FAMILY (`0x00` V-Mixer / `0x01` OHRCA), and that the
ENROLL group map is `width / 8` input groups "verified byte-for-byte across the M-200, M-300
and M-5000 golden enrols (8ch=1x41, 16ch=2x41, 32ch=4x41)".

- **Author:** Pau Aliagas <linuxnow@gmail.com>
- **Instrument:** `tools/group_map_scan` (this lane) over `reac-captures`, 96 pcap files,
  4 960 094 REAC frames. It strips the 802.1Q tag the 2026-09 sessions carry — without that
  `reac_frame_is_reac` rejects every frame of them and the scan reads clean and empty — and
  prints a per-talker census beside every result, because the findings here are mostly about
  what is NOT on the wire.

## 1. Four carriers, one value: the PACE CODE

A master declares its pace in four places, and they are one value, not four settings:

| carrier | where |
|---|---|
| cfea announce byte `[19]` | `gen_cfea` |
| ENROLL console byte | `ENROLL_BLK[8]`, block`[6]` |
| chanmap section marker `fe <code> 00` | `gen_chanmap` |
| scene body `revision` (u2le at +0x14) | `REAC_SCENE_REVISION_OFF` |

`0x00` = 48 kHz, `0x01` = 96 kHz, `0x02` = 44.1 kHz (`reac_pace_code`, now in the core library
beside the rate helpers because the master needs it as much as the pacer does).

**Evidence, with a within-desk control.** The same M-200 (`c9:cc:03`, one MAC) writes cfea
pace `0x00` in 24 043 announces and chanmap marker `0x00` in 3 133 marker slots while mastering
at 48 kHz, and `0x02` in 145 announces / 18 marker slots while mastering at 44.1 kHz; its scene
`revision` reads `0x0000` in 1 550 headers at 48 kHz and `0x0002` at 44.1 kHz. The MAC does not
change; the clock does. An **S-1608 in master mode** pacing 96 kHz writes marker `0x01` — a
stagebox is not a console generation, so the byte cannot be a family. Across the corpus every
talker's marker byte and cfea byte agree.

**The defect this closes.** Two of the four carriers were stamped `console_field ? 0x01 : 0x00`.
A boolean squash is invisible while the field only holds 0 or 1 — every test and every rig
session ran 48 or 96 kHz — and maps the 44.1 kHz code 2 onto the 96 kHz class. A 44.1 kHz master
therefore ANNOUNCED 44.1 and RECORDED 96 in the scene the box actually latches, and sent the
96 kHz chanmap marker with it. All four now read one accessor over the one value the pacer
derives from the frame rate.

**Open:** the ENROLL console byte at 44.1 kHz is still unmeasured. There is no box enrolment in
either 44.1 kHz capture — zero group-map frames, against 43 from the same desk at 48 kHz — so
`0x02` there is this law applied, not a capture. The measurement is an M-200 mastering at
44.1 kHz through a box enrolment; read block`[6]` of its `cdea 01 03 000d`.

## 2. A 16-input box gets ONE input group

103 group-map frames exist in the corpus, in four shapes, and none of them is `2 x 0x41`:

| box | width | map (block`[5:17]`) | frames |
|---|---|---|---|
| S-0808 `c4:dc:9c` | 8 in | `04 00 41 00 00 00 00 00 c3 c3 c3 c3` | 18 in `...__ctl2.pcap` |
| **S-1608 `c4:80:41`** | **16 in** | **the same map, byte for byte** | **13 over 6 captures** |
| S-4000S `c4:08:bc` | 32 in | `04 00 41 41 41 41 00 00 00 00 00 c3` | 2 |

The 16-input row comes from an M-200 (`c9:cc:03`) and an M-200i (`c9:cc:04`); in
`reacpw-estcommit-233158`, `reacpw-establish-20260721-234355` and
`reacpw-s1608-establish-2026-07-18` the S-1608 is the ONLY box on the wire, so the map can be
for no one else. The box declares 16 inputs in the same capture (selector `0x82`, inventory
`02 02 02 02 01 01 03x6`).

**The map is still a gate above 16.** In `matrix-m200-s4000-2026-07-24` the S-4000S's own
upstream runs 340 B (52 + 8 x 36 = 8 channels) before the desk's `4 x 0x41` and 1204 B (32
channels) after. In all six 16-input captures the S-1608's upstream is 628 B (16 channels)
throughout, under the one-group map — so one group does not narrow a 16-input box's return.

`set_enroll_width` therefore emits one input group up to 16 inputs and `width / 8` above. 24 and
40 inputs are unmeasured — no box of either width exists in the corpus — and take `width / 8`,
which is what the 32 row measures.

**Owed before this ships:** the desk's behaviour is measured; ours is not. A tone into inputs
9..16 of an S-1608 under a reac-pw master, with the box's upstream frames staying 628 B, is the
proof that our one-group map keeps 16 channels. Until then this conforms to the desk on paper
only.
