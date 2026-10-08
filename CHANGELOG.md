# Changelog

## 1.7.1 - 2026-10-08

- libreac and libreac-transport are now installed from the FreeMixer package channel: signed RPMs for
  Fedora 44 (x86_64, aarch64) and DEBs for Debian bookworm and trixie (amd64, arm64). Add the channel
  once, then install or update with dnf or apt. The libraries themselves do not change.
- The Debian packages are libreac6, libreac-dev, libreac-transport7 and libreac-transport-dev, named
  after the sonames they carry. The Fedora packages keep their names: libreac, libreac-devel,
  libreac-transport and libreac-transport-devel.

## 1.7.0 - 2026-10-07

- A connected stagebox is described only by what it says on the wire: its inputs and outputs from its
  declaration, its family from its identity page, and its name from both (S-1608, S-4000S-3208,
  S-4000S-1624, S-4000S-4000 ...). A box whose family has never been captured is named by its widths
  (REAC-0816).
- The built-in model table is now a model catalogue, used only to emulate a box and to plan a show
  offline. It never sizes or names a connected box; when the two disagree the box wins and the
  difference can be reported.
- The S-0808's name, which arrives split across two frames, is now read.
- A box that joins after the console first asked for its identity is asked again, once a second, so its
  firmware and hardware block are no longer left empty.
- A stagebox in master mode is recognised from what it announces or the width it broadcasts. A
  40-channel master that has not announced itself yet is waited on instead of being taken for a desk.
- Developers: reac_box_facts.h is new; the catalogue functions are renamed (reac_box_catalogue*),
  reac_box_master_model is gone, and discovery reports declared widths. libreac's soname is 6 and
  libreac-transport's is 7.

## 1.6.0 - 2026-09-25

- A stagebox may declare any even width from 2 to 40 channels in each direction, and a 40-channel box is
  legal. A box that returns 40 channels upstream is a box, not a desk.
- A sender that broadcasts is given a bounded time to show whether it is the desk or a stagebox, and is
  treated as a box once that time passes without a desk-only message.
- Sample-rate detection measures one stream, so a 48 kHz session heard in both directions is no longer
  read as 96 kHz.
- An identity reply is accepted only when both of its checksums close, and the plain-LE diagnostic
  decode refuses a geometry wider than the frame.
- Developers: reac_cfg.h is the one declaration of the configuration vocabulary and reac_code.h the one
  list of refusal codes. This is a source-level change; libreac's soname moves 4 to 5 and
  libreac-transport's 5 to 6.

## 1.5.0 - 2026-09-22

- A VLAN trunk names its VLANs by tagging, and the topology tap hears them: a VLAN whose stagebox is
  still cold is discovered as soon as the switch tags any frame on it, without transmitting anything.
- Developers: reac_topo_heard_vids() and REAC_TOPO_TAGGED_OTHER are added. No existing symbol changes.

## 1.4.0 - 2026-09-22

- The decisions about whether a wire is vacant and when to stop waiting for the topology tap are made by
  the library, through reac_knock.h and reac_tapwait.h, instead of by each program that links it.
  Behaviour is unchanged.

## 1.3.2 - 2026-09-21

- A capture no longer hears frames from every network interface on the machine for the instant before
  it is bound to its own. A cold cable could previously show a stagebox that was mastering another
  interface as heard on it.
- The transmit socket is bound to its interface and ignores incoming traffic, instead of queueing every
  REAC frame on the host.

## 1.0.0 - 2026-09-11

- The protocol library as proven on real Roland desks and stageboxes at 44.1, 48 and 96 kHz.

## Earlier releases

- Versions before 1.0.0 and the notes of each release in between are kept in the git history.
