# SPDX-License-Identifier: GPL-3.0-or-later
# libreac-transport — sockets, pacer, RT threads, VLAN/topology, ring, segment lock.
# Built from the same libreac-<version>.tar.gz as packaging/libreac.spec; see
# docs/design/specs/2026-09-11-reac-transport-library.md for what moved and why.
Name:           libreac-transport
Version:        0.9.0
%global abi 1
Release:        2%{?dist}
Summary:        The REAC transport layer — sockets, pacer, RT threads, VLAN scan (userspace backend)

License:        GPL-3.0-or-later
URL:            https://github.com/FreeREAC/libreac
Source0:        libreac-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(libreac) >= 0.9.0

Requires:       libreac%{?_isa} >= 0.9.0

%description
libreac-transport is the userspace-backend transport layer reac-pw used to carry directly:
AF_PACKET RX/TX, the lock-free SPSC ring, the SCHED_FIFO cadence pacer and its clock discipline,
the slave/master establishment orchestration (driven by libreac's protocol FSMs), interface
enumeration, VLAN sub-interface mint/adopt/release on a trunk port, the segment lock, and the
layered-config precedence + one-door-to-SCHED_FIFO rules. No socket type or AF_PACKET reference
appears in the public headers' call shapes for lifecycle purposes, so a future kernel-module
backend (reac-kmod) may implement the same API. It holds no Linux capability itself — a library
cannot; the binding process (reac-pw) keeps CAP_NET_RAW/CAP_NET_ADMIN and this library runs
inside that already-capable process.

%package devel
Summary:        Development files for libreac-transport
Requires:       %{name}%{?_isa} = %{version}-%{release}
Requires:       pkgconfig(libreac) >= 0.9.0

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
Requires: libreac >= 0.9.0
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
* Fri Sep 11 2026 Pau Aliagas <linuxnow@gmail.com> - 0.9.0-2
- cfea[19] is the pace code: 44.1 kHz announces 2 (was the 48 kHz code); an S-4000S
  under a 44.1 kHz master now paces 44.1 kHz instead of 48 kHz.
* Fri Sep 11 2026 Pau Aliagas <linuxnow@gmail.com> - 0.9.0-1
- First release: reac_ifscan, reac_topo, reac_vlan, reac_slave, reac_pacer, reac_tx, reac_rx,
  reac_linkmon, reac_segment_ident, reac_seglock, reac_role_swap, reac_ring, reac_rt,
  reac_pace_watch, reac_ifname, reac_conf, reac_mac and reac_carrier (renamed from reac-pw's
  local reac_link, distinct from libreac's own protocol-level reac_link) move here unchanged
  from reac-pw. See docs/design/specs/2026-09-11-reac-transport-library.md.
