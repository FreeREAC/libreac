# SPDX-License-Identifier: GPL-3.0-or-later
# libreac — Roland REAC RX core, Fedora shared library.
Name:           libreac
Version:        0.4.0
Release:        1%{?dist}
Summary:        Roland REAC wire-format core (validate, counter, 24-bit decode/encode, capture)

License:        GPL-3.0-or-later
URL:            https://github.com/FreeREAC/libreac
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc

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
for f in reac reac_decode reac_upstream reac_encode reac_capture pcap_source; do
  cc %{optflags} -fPIC -Iinclude -c src/$f.c -o $f.o
done
# %%build_ldflags carries the Fedora link flags incl. --build-id, which the
# debuginfo extraction requires (%%optflags already gave the objects -g).
# -lm: reac_encode's float->s24 rounds with lrintf.
cc %{build_ldflags} -shared -Wl,-soname,libreac.so.0 -o libreac.so.%{version} \
  reac.o reac_decode.o reac_upstream.o reac_encode.o reac_capture.o pcap_source.o -lm

%install
install -Dm0755 libreac.so.%{version} %{buildroot}%{_libdir}/libreac.so.%{version}
ln -s libreac.so.%{version} %{buildroot}%{_libdir}/libreac.so.0
ln -s libreac.so.0          %{buildroot}%{_libdir}/libreac.so
for h in reac reac_decode reac_braid reac_sample reac_upstream reac_encode reac_capture pcap_source; do
  install -Dm0644 include/reac/$h.h %{buildroot}%{_includedir}/reac/$h.h
done
mkdir -p %{buildroot}%{_libdir}/pkgconfig
cat > %{buildroot}%{_libdir}/pkgconfig/libreac.pc <<PC
prefix=%{_prefix}
libdir=%{_libdir}
includedir=%{_includedir}

Name: libreac
Description: Roland REAC wire-format core
Version: %{version}
Libs: -L\${libdir} -lreac
Cflags: -I\${includedir}
PC

%files
%license LICENSE
%{_libdir}/libreac.so.%{version}
%{_libdir}/libreac.so.0

%files devel
%{_includedir}/reac/reac.h
%{_includedir}/reac/reac_decode.h
%{_includedir}/reac/reac_braid.h
%{_includedir}/reac/reac_sample.h
%{_includedir}/reac/reac_upstream.h
%{_includedir}/reac/reac_encode.h
%{_includedir}/reac/reac_capture.h
%{_includedir}/reac/pcap_source.h
%{_libdir}/libreac.so
%{_libdir}/pkgconfig/libreac.pc

%changelog
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
