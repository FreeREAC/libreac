# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# OpenWrt feed recipe for libreac — the shared REAC RX core. Builds a shared
# object + installs headers/lib into the staging dir so the FreeREAC C tools
# (reac-aes67, reac-pw) can compile + link against it (DEPENDS +libreac).
include $(TOPDIR)/rules.mk

PKG_NAME:=libreac
# THE VERSION LIVES IN include/reac/reac.h. Read it rather than keeping a fourth
# copy: this one said 0.5.0 while the spec said 0.6.0 and the library reported a
# third thing.
PKG_VERSION:=$(shell awk '/^\#define LIBREAC_VERSION_MAJOR/{ma=$$3} /^\#define LIBREAC_VERSION_MINOR/{mi=$$3} /^\#define LIBREAC_VERSION_PATCH/{pa=$$3} END{printf "%s.%s.%s", ma, mi, pa}' $(CURDIR)/../../include/reac/reac.h)
ifeq ($(PKG_VERSION),..)
$(error could not read LIBREAC_VERSION_* from include/reac/reac.h)
endif
PKG_RELEASE:=1
PKG_MAINTAINER:=Pau Aliagas <linuxnow@gmail.com>
PKG_LICENSE:=GPL-3.0-or-later

include $(INCLUDE_DIR)/package.mk

define Package/libreac
  SECTION:=libs
  CATEGORY:=Libraries
  TITLE:=Shared REAC wire-format core (FreeREAC)
  URL:=https://github.com/FreeREAC/libreac
  ABI_VERSION:=0
endef

define Package/libreac/description
  Shared REAC (EtherType 0x8819) byte-layout core used by the FreeREAC C tools
  (reac-aes67, reac-pw): the rate-invariant mode descriptors + packet-rate
  detection, frame validation, the braided box-upstream decode + braid/sample
  layout oracles (plus the legacy plain-LE downstream decode), the braided
  ENCODE and the downstream frame builder, the OHRCA +2 trailer rule, live
  AF_PACKET capture, and the offline pcap reader.
  See github.com/FreeREAC/reac-protocol.
endef

# Source is vendored under ./src + ./include (populated from the repo at build).
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)/reac
	$(CP) ./src/reac.c ./src/reac_decode.c ./src/reac_upstream.c ./src/reac_encode.c ./src/reac_capture.c ./src/pcap_source.c $(PKG_BUILD_DIR)/
	$(CP) ./include/reac/reac.h ./include/reac/reac_decode.h ./include/reac/reac_braid.h ./include/reac/reac_sample.h ./include/reac/reac_upstream.h ./include/reac/reac_encode.h ./include/reac/reac_capture.h ./include/reac/pcap_source.h $(PKG_BUILD_DIR)/reac/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c11 -fPIC -I$(PKG_BUILD_DIR) -c $(PKG_BUILD_DIR)/reac.c -o $(PKG_BUILD_DIR)/reac.o
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c11 -fPIC -I$(PKG_BUILD_DIR) -c $(PKG_BUILD_DIR)/reac_decode.c -o $(PKG_BUILD_DIR)/reac_decode.o
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c11 -fPIC -I$(PKG_BUILD_DIR) -c $(PKG_BUILD_DIR)/reac_upstream.c -o $(PKG_BUILD_DIR)/reac_upstream.o
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c11 -fPIC -I$(PKG_BUILD_DIR) -c $(PKG_BUILD_DIR)/reac_encode.c -o $(PKG_BUILD_DIR)/reac_encode.o
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c11 -fPIC -I$(PKG_BUILD_DIR) -c $(PKG_BUILD_DIR)/reac_capture.c -o $(PKG_BUILD_DIR)/reac_capture.o
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c11 -fPIC -I$(PKG_BUILD_DIR) -c $(PKG_BUILD_DIR)/pcap_source.c -o $(PKG_BUILD_DIR)/pcap_source.o
	$(TARGET_CC) $(TARGET_LDFLAGS) -shared -Wl,-soname,libreac.so.0 \
		-o $(PKG_BUILD_DIR)/libreac.so.$(PKG_VERSION) \
		$(PKG_BUILD_DIR)/reac.o $(PKG_BUILD_DIR)/reac_decode.o \
		$(PKG_BUILD_DIR)/reac_upstream.o $(PKG_BUILD_DIR)/reac_encode.o \
		$(PKG_BUILD_DIR)/reac_capture.o $(PKG_BUILD_DIR)/pcap_source.o -lm
	ln -sf libreac.so.$(PKG_VERSION) $(PKG_BUILD_DIR)/libreac.so.0
	ln -sf libreac.so.0 $(PKG_BUILD_DIR)/libreac.so
endef

# headers + lib into the SDK staging dir, so dependent packages can build against it
define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/reac
	$(CP) ./include/reac/reac.h ./include/reac/reac_decode.h ./include/reac/reac_braid.h ./include/reac/reac_sample.h ./include/reac/reac_upstream.h ./include/reac/reac_encode.h ./include/reac/reac_capture.h ./include/reac/pcap_source.h $(1)/usr/include/reac/
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_BUILD_DIR)/libreac.so* $(1)/usr/lib/
endef

# runtime shared object on the device
define Package/libreac/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_BUILD_DIR)/libreac.so.$(PKG_VERSION) $(1)/usr/lib/
	ln -sf libreac.so.$(PKG_VERSION) $(1)/usr/lib/libreac.so.0
endef

$(eval $(call BuildPackage,libreac))
