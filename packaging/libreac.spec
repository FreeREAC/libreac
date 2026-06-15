# SPDX-License-Identifier: GPL-3.0-or-later
# libreac — Roland REAC frame primitives, Fedora shared library.
%global debug_package %{nil}
Name:           libreac
Version:        0.1.0
Release:        1%{?dist}
Summary:        Roland REAC frame primitives (validate, counter, 24-bit decode)

License:        GPL-3.0-or-later
URL:            https://github.com/FreeREAC/libreac
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc

%description
libreac is the shared core of the REAC tools: recognise a REAC frame (EtherType
0x8819), read its sequence counter, and decode the 24-bit plain-LE audio. It is
consumed by reac-aes67 (the REAC->AES67 bridge) and reac-pw (the PipeWire-native
endpoint), which link it dynamically.

%package devel
Summary:        Development files for libreac
Requires:       %{name}%{?_isa} = %{version}-%{release}

%description devel
Headers and pkg-config for building against libreac.

%prep
%autosetup -n %{name}-%{version}

%build
cc %{optflags} -fPIC -Iinclude -c src/reac.c -o reac.o
cc -shared -Wl,-soname,libreac.so.0 -o libreac.so.0.1.0 reac.o

%install
install -Dm0755 libreac.so.0.1.0 %{buildroot}%{_libdir}/libreac.so.0.1.0
ln -s libreac.so.0.1.0 %{buildroot}%{_libdir}/libreac.so.0
ln -s libreac.so.0     %{buildroot}%{_libdir}/libreac.so
install -Dm0644 include/reac/reac.h %{buildroot}%{_includedir}/reac/reac.h
mkdir -p %{buildroot}%{_libdir}/pkgconfig
cat > %{buildroot}%{_libdir}/pkgconfig/libreac.pc <<PC
prefix=%{_prefix}
libdir=%{_libdir}
includedir=%{_includedir}

Name: libreac
Description: Roland REAC frame primitives
Version: %{version}
Libs: -L\${libdir} -lreac
Cflags: -I\${includedir}
PC

%files
%license LICENSE
%{_libdir}/libreac.so.0.1.0
%{_libdir}/libreac.so.0

%files devel
%{_includedir}/reac/reac.h
%{_libdir}/libreac.so
%{_libdir}/pkgconfig/libreac.pc

%changelog
* Mon Jun 15 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.0-1
- Initial Fedora package: libreac shared library + -devel.
