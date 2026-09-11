# SPDX-License-Identifier: GPL-3.0-or-later
# libreac-transport — sockets, pacer, RT threads, VLAN/topology, ring, segment lock.
# Built from the same libreac-<version>.tar.gz as packaging/libreac.spec; see
# docs/design/specs/2026-09-11-reac-transport-library.md for what moved and why.
Name:           libreac-transport
Version:        1.0.0
%global abi 2
Release:        1%{?dist}
Summary:        The REAC transport layer — sockets, pacer, RT threads, VLAN scan (userspace backend)

License:        GPL-3.0-or-later
URL:            https://github.com/FreeREAC/libreac
Source0:        libreac-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(libreac) >= 1.0.0

Requires:       libreac%{?_isa} >= 1.0.0

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
Requires:       pkgconfig(libreac) >= 1.0.0

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
Requires: libreac >= 1.0.0
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
