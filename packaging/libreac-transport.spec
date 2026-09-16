# SPDX-License-Identifier: GPL-3.0-or-later
# libreac-transport — sockets, pacer, RT threads, VLAN/topology, ring, segment lock.
# Built from the same libreac-<version>.tar.gz as packaging/libreac.spec; see
# docs/design/specs/2026-09-11-reac-transport-library.md for what moved and why.
Name:           libreac-transport
Version:        1.1.5
%global abi 4
Release:        1%{?dist}
Summary:        The REAC transport layer — sockets, pacer, RT threads, VLAN scan (userspace backend)

License:        GPL-3.0-or-later
URL:            https://github.com/FreeREAC/libreac
Source0:        libreac-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(libreac) >= 1.1.0

Requires:       libreac%{?_isa} >= 1.1.0

%description
libreac-transport is the REAC transport layer: AF_PACKET frame RX/TX over a lock-free
ring, a SCHED_FIFO cadence pacer with clock discipline, network interface and VLAN
scanning, segment locking, and slave/master establishment orchestration built on
libreac's protocol state machines. The public API carries no socket type, so an
alternate backend can implement the same shape; the library holds no Linux capability
itself, that belongs to the process that links it.

%package devel
Summary:        Development files for libreac-transport
Requires:       %{name}%{?_isa} = %{version}-%{release}
Requires:       pkgconfig(libreac) >= 1.1.0

%description devel
Headers and pkg-config for building against libreac-transport.

%prep
%autosetup -n libreac-%{version}

%build
# See packaging/vendor/README.md: two of these headers (reac_pacer.h, reac_role_swap.h)
# #include a reac-pw header for pure declarations only (the design spec's own named seam,
# not an oversight). The vendored snapshot lets this SRPM build without a reac-pw checkout;
# refresh it by hand when reac-pw's two headers change, until the real header split lands.
for f in transport/src/*.c; do
  cc %{optflags} -fPIC -D_GNU_SOURCE -Iinclude -Ipackaging/vendor/reac-pw-headers \
     -c "$f" -o "$(basename "$f" .c).o"
done
# -lreac (via pkg-config, BuildRequires above) resolves every reac_ctrl_*/reac_hunt_*/
# reac_master_*/... symbol the transport calls into libreac for. --no-undefined is the
# same gate libreac.spec's own %build carries: a missing symbol fails HERE, at package
# build, not at a consumer's exec.
cc %{build_ldflags} -shared -Wl,-soname,libreac-transport.so.%{abi} -Wl,--no-undefined \
  -o libreac-transport.so.%{version} *.o $(pkg-config --libs libreac) -lm -lpthread

%install
install -Dm0755 libreac-transport.so.%{version} %{buildroot}%{_libdir}/libreac-transport.so.%{version}
ln -s libreac-transport.so.%{version} %{buildroot}%{_libdir}/libreac-transport.so.%{abi}
ln -s libreac-transport.so.%{abi}     %{buildroot}%{_libdir}/libreac-transport.so
for h in include/reac/transport/*.h; do
  install -Dm0644 "$h" %{buildroot}%{_includedir}/reac/transport/"$(basename "$h")"
done
mkdir -p %{buildroot}%{_libdir}/pkgconfig
cat > %{buildroot}%{_libdir}/pkgconfig/libreac-transport.pc <<PC
prefix=%{_prefix}
exec_prefix=\${prefix}
libdir=\${exec_prefix}/%{_lib}
includedir=\${prefix}/include

Name: libreac-transport
Description: The REAC transport layer (sockets, pacer, RT threads, VLAN scan)
Version: %{version}
Requires: libreac >= 1.1.0
Libs: -L\${libdir} -lreac-transport -lpthread -lm
Cflags: -I\${includedir}
PC

%files
%license LICENSE
%{_libdir}/libreac-transport.so.%{version}
%{_libdir}/libreac-transport.so.%{abi}

%files devel
%dir %{_includedir}/reac/transport
%{_includedir}/reac/transport/*.h
%{_libdir}/libreac-transport.so
%{_libdir}/pkgconfig/libreac-transport.pc

%changelog
* Wed Sep 16 2026 Pau Aliagas <linuxnow@gmail.com> - 1.1.5-1
- reac_link_admin(): set a netdev's IFF_UP in EITHER direction, over the rtnetlink socket
  reac_vlan.c already owns; reac_vlan_up() is now one line of it. A stagebox leaves its
  dropped state on PHY LINK-UP and on nothing else (REAC-PROTOCOL-FROM-SOURCE 10.2), so a
  master whose box went quiet has no frame that brings it back -- on 2026-09-16 that cost a
  live segment 73 minutes of correct probing into silence. The policy is reac-pw's
  reac_wake; this is only the write. ADDED SYMBOL, no struct touched: LIBREAC_ABI stays 3
  and tests/abi-layout.inc is unchanged.
- The PROBING watchdog now prints the number of COMPLETED scene pushes and stops telling the
  operator "do not bounce it yet". That sentence is true of the S-4000S it was measured from
  and false of a box that has DROPPED; on 2026-09-16 it kept a live segment waiting an hour
  for a frame that could not come. The push count is the only number that separates "our own
  transfer never finished" from "the far end ignored a whole one", and nothing printed it.
* Tue Sep 16 2026 Pau Aliagas <linuxnow@gmail.com> - 1.1.4-1
- reac_hunt: a segment PINNED master with a stagebox already mastering the wire now JOINS it
  as a slave instead of refusing (operator ruling 2026-09-16, "enroll any box, master or
  slave"). The rig measured the refusal twice: a door with no audio and an operator reading
  "not detected". REAC_HUNT_REFUSED is left for its one remaining case, a rival whose geometry
  has never been captured. Behaviour only -- same ABI (LIBREAC_ABI 3), no struct, no symbol.
* Mon Sep 14 2026 Pau Aliagas <linuxnow@gmail.com> - 1.1.3-1
- ETF IS THE DEFAULT PACING BACKEND (operator ruling, 2026-09-14). Measured on the TX device,
  60 s per arm, one S-4000S-3208 per link: interval sd 28.5 -> 2.7 us (PCI VLAN) and
  15.3 -> 1.9 us (USB direct), p99.9 595 -> 136 and 308 -> 131 us, late slots 27-37/s ->
  0.45/s, with no rise in the daemon's own CPU. REACPW_PACER=thread opts out.
- A precondition the machine cannot meet now depends on WHO ASKED. Explicit REACPW_PACER=etf
  still FAILS the open and names the code. The DEFAULT logs one loud line naming the refusal
  and its fix, runs the thread backend, and publishes the refusal
  (reac_pacer_backend_refusal) -- a fallback the operator cannot see is the silent no-op the
  backend exists to avoid.
- NEW: <reac/transport/reac_etf_qdisc.h>. Install, remove and read back the ETF qdisc over
  rtnetlink -- no tc(8), no subprocess. del-then-add, because etf supports no change
  operation. Every message carries NLM_F_ACK and the ack is READ: a netlink write the kernel
  refuses returns the same byte count as one it accepted. The message builders are asserted
  byte for byte against what iproute2 6.17.0 puts on the same socket (strace capture inside
  `unshare -rn`, in tests/test_reac_etf_qdisc.c).
- Refusals are classified by ERRNO, never by the kernel's extack string: EPERM is a
  capability, ENOENT a missing sch_etf, EOPNOTSUPP the device, EINVAL the parameters. The
  test requires all four fixes to differ.
- reac_etf_qdisc_probe becomes the installed reac_etf_qdisc_state: the pacer's open-time
  check and the daemon's what-is-there-now are one reading. No public struct grows;
  libreac-transport.so.4 is unchanged.
* Mon Sep 14 2026 Pau Aliagas <linuxnow@gmail.com> - 1.1.2-1
- WITHDRAWS 1.1.1, WHICH WAS AN UNANNOUNCED ABI BREAK. 1.1.1 added one `int` at offset 1712
  of `struct reac_master` -- a per-cycle cursor, entirely internal -- and moved every member
  behind it four bytes: headamp_src 14304 -> 14308, box_mac 14344 -> 14348, and through the
  embedding reac_pacer.recognized_box 15232 -> 15236. sizeof(struct reac_master) went
  14456 -> 14464 and sizeof(struct reac_pacer) 24312 -> 24320, with LIBREAC_ABI and both
  sonames unmoved. reac-pw 1.0.4, built against 1.1.0, then read a POINTER from the wrong
  offset and dereferenced it: SEGV about 7 s after every start, 99 restarts on the live rig
  before the rollback. Every test was green, because every test is rebuilt against the
  headers it is testing.
- The 44.1 kHz burst cadence 1.1.1 shipped is KEPT and is now computed rather than stored:
  burst_slot()/burst_index_at() place chunk k at the rounded k*fps/500, so the transfer
  still spans the desk's measured 2511 slots and the struct does not grow at all. The
  emitted frames are byte-identical to 1.1.1's at 44.1, 48 and 96 kHz.
- THE RATCHET THAT WOULD HAVE CAUGHT IT: tests/abi-layout.inc records sizeof, _Alignof and
  every member offset of all 61 public structs (566 offsets), generated from DWARF by
  tools/gen-abi-layout.py; tests/test_abi_layout.c re-measures them with offsetof in
  `make test`. A layout that moves is red unless LIBREAC_ABI moved and the table was
  regenerated in the same change.
- tools/fake_box: a wire-level linked-silent box built from the desk-arrival capture's own
  bytes, which is what reproduced the crash off the rig (a veth in a private namespace) and
  what proves it gone.
* Mon Sep 14 2026 Pau Aliagas <linuxnow@gmail.com> - 1.1.1-1
- Follows libreac 1.1.1; the pacer no longer tells the operator to bounce a linked, silent box.
* Mon Sep 14 2026 Pau Aliagas <linuxnow@gmail.com> - 1.1.0-1
- SONAME 3 -> 4, for the same reason .so.3 moved: `struct reac_pacer` EMBEDS a libreac
  struct that grew. libreac 1.1.0 decodes the identity page's 0x0600 record as the box's
  REAC version, so struct reac_identity goes 30 -> 38 bytes; reac_pacer carries one as
  rx_identity and goes 24304 -> 24312, with every field after rx_identity (identity_seq,
  declared_in/out, drops, the whole discovery half) shifted by 8. Measured with offsetof
  against both header trees, x86-64.
- The libreac floor rises to 1.1.0 in all four places (BuildRequires, Requires, the devel
  Requires and the generated pkg-config), because a transport built against the 38-byte
  identity cannot run against the 30-byte one.
- reac_pacer_read_identity() hands back a REAC version alongside the firmware; the
  seqlock around rx_identity is unchanged and still covers the whole struct.
* Mon Sep 14 2026 Pau Aliagas <linuxnow@gmail.com> - 1.0.3-1
- The tap role: reac_tap survey and serve (listen, never transmit) for a segment another
  master owns — one stream for the desk downstream, one per box. Same ABI, new symbols.
* Sun Sep 13 2026 Pau Aliagas <linuxnow@gmail.com> - 1.0.2-1
- Follows libreac 1.0.2 (the pace code is shared with the core). Same ABI.
* Sat Sep 12 2026 Pau Aliagas <linuxnow@gmail.com> - 1.0.1-1
- Follows libreac 1.0.1: the slave state carries the bounded courtship (struct reac_slave
  embeds struct reac_fsm), so the soname moves to libreac-transport.so.3.
* Fri Sep 11 2026 Pau Aliagas <linuxnow@gmail.com> - 1.0.0-1
- 1.0: opaque OS handle, pace code for 44.1/48/96 kHz, VLAN trunk segments. Same ABI as
  0.9.1 (libreac-transport.so.2).
* Fri Sep 11 2026 Pau Aliagas <linuxnow@gmail.com> - 0.9.1-1
- The OS descriptor leaves every installed header: reac_tx, reac_pacer, reac_slave,
  reac_seglock, reac_ifscan, reac_linkmon and the topo tap hold an opaque struct reac_handle
  the library allocates in open/claim and frees in close/release (spec §3). Struct layouts
  change, so the soname moves to .so.2. reac_seglock_init/_held and reac_topo_tap_fd added.
* Fri Sep 11 2026 Pau Aliagas <linuxnow@gmail.com> - 0.9.0-2
- cfea[19] is the pace code: 44.1 kHz announces 2 (was the 48 kHz code); an S-4000S
  under a 44.1 kHz master now paces 44.1 kHz instead of 48 kHz.
* Fri Sep 11 2026 Pau Aliagas <linuxnow@gmail.com> - 0.9.0-1
- First release: reac_ifscan, reac_topo, reac_vlan, reac_slave, reac_pacer, reac_tx, reac_rx,
  reac_linkmon, reac_segment_ident, reac_seglock, reac_role_swap, reac_ring, reac_rt,
  reac_pace_watch, reac_ifname, reac_conf, reac_mac and reac_carrier (renamed from reac-pw's
  local reac_link, distinct from libreac's own protocol-level reac_link) move here unchanged
  from reac-pw. See docs/design/specs/2026-09-11-reac-transport-library.md.
