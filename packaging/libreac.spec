# SPDX-License-Identifier: GPL-3.0-or-later
# libreac — Roland REAC RX core, Fedora shared library.
Name:           libreac
Version:        0.7.0
# THE SONAME'S MAJOR, and it is not decoration. rpm generates this package's
# `provides` (libreac.so.N()(64bit)) and every consumer's runtime `requires`
# from it, so bumping it is what makes a mismatched pair refuse to install
# instead of failing at exec time with `undefined symbol`. It tracks
# LIBREAC_ABI in include/reac/reac.h -- packaging/make-tarball.sh refuses to
# build a tarball when this copy and the header disagree, which is the only
# moment the copy can be caught.
%global abi 1
Release:        1%{?dist}
Summary:        Roland REAC wire-format core (validate, counter, 24-bit decode/encode, capture)

License:        GPL-3.0-or-later
URL:            https://github.com/FreeREAC/libreac
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make

%description
libreac is the shared byte-layout core of the REAC tools: recognise a REAC frame
(EtherType 0x8819), read its sequence counter, detect the sample rate, decode the
24-bit audio — the braided box upstream (reac_upstream) over the braid/sample
oracles (reac_braid.h / reac_sample.h) plus the legacy plain-LE downstream path —
ENCODE it back (reac_encode: the braided audio region in both directions and the
40-channel downstream broadcast frame), strip the OHRCA +2 CRC trailer, and read
frames from a live AF_PACKET capture or an offline pcap. It is consumed by
reac-aes67 (the REAC->AES67 bridge) and reac-pw (the PipeWire-native endpoint),
which link it dynamically.

%package devel
Summary:        Development files for libreac
Requires:       %{name}%{?_isa} = %{version}-%{release}

%description devel
Headers and pkg-config for building against libreac.

%prep
%autosetup -n %{name}-%{version}

%build
# Every src/*.c, never a hand-kept list. The list form had drifted twice: it was
# still naming six files after src/reac_ports.c and src/reac_ctrlblk.c landed, so
# the shared object shipped without reac_ports_parse, reac_headamp_base and the
# whole reac_ctrl_*/reac_headamp_* control-block surface, while the -devel package
# happily installed headers declaring them. Nothing failed at build time -- a
# consumer only found out at link.
for f in src/*.c; do
  cc %{optflags} -fPIC -Iinclude -c "$f" -o "$(basename "$f" .c).o"
done
# %%build_ldflags carries the Fedora link flags incl. --build-id, which the
# debuginfo extraction requires (%%optflags already gave the objects -g).
# -lm: reac_encode's float->s24 rounds with lrintf.
cc %{build_ldflags} -shared -Wl,-soname,libreac.so.%{abi} -o libreac.so.%{version} \
  *.o -lm

%install
install -Dm0755 libreac.so.%{version} %{buildroot}%{_libdir}/libreac.so.%{version}
ln -s libreac.so.%{version} %{buildroot}%{_libdir}/libreac.so.%{abi}
ln -s libreac.so.%{abi}     %{buildroot}%{_libdir}/libreac.so
# Same rule as %%build: every public header, derived. reac_ctrlblk.h and
# reac_ports.h were missing from the old hand-kept list.
for h in include/reac/*.h; do
  install -Dm0644 "$h" %{buildroot}%{_includedir}/reac/"$(basename "$h")"
done
mkdir -p %{buildroot}%{_libdir}/pkgconfig
cat > %{buildroot}%{_libdir}/pkgconfig/libreac.pc <<PC
prefix=%{_prefix}
exec_prefix=\${prefix}
libdir=\${exec_prefix}/%{_lib}
includedir=\${prefix}/include

Name: libreac
Description: Roland REAC wire-format core
Version: %{version}
Libs: -L\${libdir} -lreac
Cflags: -I\${includedir}
PC

%check
# libreac's own suite, against the very objects %%build produced (the Makefile
# finds them up to date and only archives them). Without this the RPM was built
# and shipped without one assertion ever running -- the tests were not even in
# the tarball.
make test

%files
%license LICENSE
%{_libdir}/libreac.so.%{version}
%{_libdir}/libreac.so.%{abi}

%files devel
# The whole directory, so a new public header ships the day it lands instead of
# waiting for someone to remember this list.
%dir %{_includedir}/reac
%{_includedir}/reac/*.h
%{_libdir}/libreac.so
%{_libdir}/pkgconfig/libreac.pc

%changelog
* Sun Aug 23 2026 Pau Aliagas <linuxnow@gmail.com> - 0.7.0-1
- API BREAK, and this is the release that admits it. The identity record is one
  message built by the identity-first surface; reac_ctrl_build_name_frame() and
  reac_ctrl_build_extra_frame() are removed. Head-amp phantom is per channel on
  the wire, measured 2026-08-23, and the control block is classified by link,
  segment and opcode rather than by frame length.
- SONAME 0 -> 1. The break above shipped once already under 0.6.0 with soname 0
  and unchanged version digits. Nothing could see it: reac-pw's `>= 0.6.0`
  floor accepted old and new alike, the identical NEVRA made `rpm -U` a no-op,
  and the installed /usr/bin/reac-pw loaded the new libreac.so.0 and died on
  `undefined symbol`. With the soname moved the two are co-installable, rpm's
  generated requires refuse a mismatched pair at INSTALL time, and a stale
  binary keeps loading .so.0 until it is replaced instead of breaking.
- The soname's major now has one home, LIBREAC_ABI in include/reac/reac.h, and
  %%global abi tracks it; make-tarball.sh refuses a tarball when the two
  disagree, the same gate that already guarded the version digits.
- Consumers must rebuild: reac-pw's floor is raised to >= 0.7.0 in the same
  change. The library and any binary linked against it go in TOGETHER.

* Sat Aug 22 2026 Pau Aliagas <linuxnow@gmail.com> - 0.6.0-1
- The control-block and port-declaration core ships: src/reac_ctrlblk.c
  (reac_ctrl_* frame builders and parsers, the checksum stamp/verify pair, the
  head-amp record surface reac_headamp_*) and src/reac_ports.c
  (reac_ports_parse, reac_headamp_base). Both had been in the source tree and in
  -devel's headers for some time while the shared object did not contain them:
  the spec named its objects, its headers and its %%files entries in three
  separate hand-kept lists and all three had drifted. They are now derived from
  src/*.c and include/reac/*.h, and %%files devel owns %%{_includedir}/reac.
- %%check runs the suite (8 binaries) during the build. It did not run at all
  before, because make-tarball.sh did not ship tests/ or the Makefile.
- The SENS curve is one flat dB per step; REAC_HEADAMP_SENS_* publish the three
  constants the schema is held to.

* Wed Jul 29 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.0-1
- BEHAVIOUR CHANGE: reac_decode() decodes the channel-pair BRAID, the layout
  reac_downstream_build() writes. Up to 0.4.0 it read plain LE sample-major, so
  libreac could not read back a frame it had just built — 0 of 480 samples of a
  self-built 1492 B frame agreed (#13). The signature is unchanged, so existing
  callers become correct without a source change; there is one downstream layout
  for every mixer generation and the per-generation plain-LE/braid split that
  kept plain LE the default is refuted, not open.
- Plain LE stays reachable as reac_decode_plain_le(), byte-identical to the
  pre-0.5.0 reac_decode() and pinned as such by a digest computed from the 0.4.0
  code. Diagnostic only: historical captures and the mid-byte lane-shift
  artefact. New suite tests/test_decode.c pins the encode->decode round trip
  exact at all three rates and rules out cross-wiring.
* Wed Jul 29 2026 Pau Aliagas <linuxnow@gmail.com> - 0.4.0-1
- Encode side lands here: reac_encode.{h,c} — reac_braid_encode() (the braided
  audio region, the exact inverse of reac_upstream_decode, used in BOTH
  directions) and reac_downstream_build() (the whole 1492 B master broadcast).
  Moved from reac-pw (reac_tx_build + reac_ctrl.c's static place_braided_audio,
  which were the same loop written twice) so the wire format has one home in
  both directions. Proven byte-identical to the pre-move encoder over a
  deterministic corpus; the digest is pinned in tests/test_encode.c and in
  reac-pw's suite. AF_PACKET emission, the pacer and the control-block/checksum
  handshake deliberately stay in reac-pw.
* Tue Jul 28 2026 Pau Aliagas <linuxnow@gmail.com> - 0.3.0-1
- Braid codec consolidated here as the single layout oracle: reac_braid.h
  (variable-width channel-pair byte map), reac_sample.h (f32<->s24le pair),
  reac_upstream.{h,c} (box-upstream decode, moved from reac-pw with its
  captured-frame fixtures), REAC_FRAME_BYTES_OHRCA + reac_frame_clean_len
  (one home for the OHRCA +2 CRC trailer rule). Plain-LE reac_decode kept
  byte-identical but marked contested/diagnostic.
* Tue Jul 21 2026 Pau Aliagas <linuxnow@gmail.com> - 0.2.0-1
- Absorb the REAC decode core: reac_decode, reac_capture, pcap_source now ship in
  libreac (single source of truth for reac-aes67 + reac-pw).
* Mon Jun 15 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.0-1
- Initial Fedora package: libreac shared library + -devel.
