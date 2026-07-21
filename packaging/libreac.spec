# SPDX-License-Identifier: GPL-3.0-or-later
# libreac — Roland REAC RX core, Fedora shared library.
%global debug_package %{nil}
Name:           libreac
Version:        0.2.0
Release:        1%{?dist}
Summary:        Roland REAC RX core (validate, counter, 24-bit decode, capture)

License:        GPL-3.0-or-later
URL:            https://github.com/FreeREAC/libreac
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc

%description
libreac is the shared RX core of the REAC tools: recognise a REAC frame (EtherType
0x8819), read its sequence counter, detect the sample rate, decode the 24-bit
plain-LE audio, and read frames from a live AF_PACKET capture or an offline pcap.
It is consumed by reac-aes67 (the REAC->AES67 bridge) and reac-pw (the
PipeWire-native endpoint), which link it dynamically.

%package devel
Summary:        Development files for libreac
Requires:       %{name}%{?_isa} = %{version}-%{release}

%description devel
Headers and pkg-config for building against libreac.

%prep
%autosetup -n %{name}-%{version}

%build
for f in reac reac_decode reac_capture pcap_source; do
  cc %{optflags} -fPIC -Iinclude -c src/$f.c -o $f.o
done
cc -shared -Wl,-soname,libreac.so.0 -o libreac.so.%{version} \
  reac.o reac_decode.o reac_capture.o pcap_source.o

%install
install -Dm0755 libreac.so.%{version} %{buildroot}%{_libdir}/libreac.so.%{version}
ln -s libreac.so.%{version} %{buildroot}%{_libdir}/libreac.so.0
ln -s libreac.so.0          %{buildroot}%{_libdir}/libreac.so
for h in reac reac_decode reac_capture pcap_source; do
  install -Dm0644 include/reac/$h.h %{buildroot}%{_includedir}/reac/$h.h
done
mkdir -p %{buildroot}%{_libdir}/pkgconfig
cat > %{buildroot}%{_libdir}/pkgconfig/libreac.pc <<PC
prefix=%{_prefix}
libdir=%{_libdir}
includedir=%{_includedir}

Name: libreac
Description: Roland REAC RX core
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
%{_includedir}/reac/reac_capture.h
%{_includedir}/reac/pcap_source.h
%{_libdir}/libreac.so
%{_libdir}/pkgconfig/libreac.pc

%changelog
* Tue Jul 21 2026 Pau Aliagas <linuxnow@gmail.com> - 0.2.0-1
- Absorb the REAC decode core: reac_decode, reac_capture, pcap_source now ship in
  libreac (single source of truth for reac-aes67 + reac-pw).
* Mon Jun 15 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.0-1
- Initial Fedora package: libreac shared library + -devel.
