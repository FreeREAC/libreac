# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# OpenWrt feed recipe for libreac — the shared REAC protocol library. Builds a
# shared object + installs headers/lib into the staging dir so the FreeREAC C
# tools (reac-aes67, reac-repacer) can compile + link against it (DEPENDS +libreac).
include $(TOPDIR)/rules.mk

PKG_NAME:=libreac
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_MAINTAINER:=Pau Aliagas <linuxnow@gmail.com>
PKG_LICENSE:=GPL-3.0-or-later

include $(INCLUDE_DIR)/package.mk

define Package/libreac
  SECTION:=libs
  CATEGORY:=Libraries
  TITLE:=Shared REAC protocol library (FreeREAC)
  URL:=https://github.com/FreeREAC/libreac
  ABI_VERSION:=0
endef

define Package/libreac/description
  Shared REAC (EtherType 0x8819) protocol constants and helpers used by the
  FreeREAC C tools (reac-aes67, reac-repacer): the rate-invariant mode descriptors
  (40 ch x 12 samples; 44.1/48/96 kHz), packet-rate detection (pps -> sample rate),
  and frame validation. See github.com/FreeREAC/reac-protocol.
endef

# Source is vendored under ./src + ./include (populated from the repo at build).
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)/reac
	$(CP) ./src/reac.c $(PKG_BUILD_DIR)/reac.c
	$(CP) ./include/reac/reac.h $(PKG_BUILD_DIR)/reac/reac.h
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c11 -fPIC -I$(PKG_BUILD_DIR) \
		-c $(PKG_BUILD_DIR)/reac.c -o $(PKG_BUILD_DIR)/reac.o
	$(TARGET_CC) $(TARGET_LDFLAGS) -shared -Wl,-soname,libreac.so.0 \
		-o $(PKG_BUILD_DIR)/libreac.so.$(PKG_VERSION) $(PKG_BUILD_DIR)/reac.o
	ln -sf libreac.so.$(PKG_VERSION) $(PKG_BUILD_DIR)/libreac.so.0
	ln -sf libreac.so.0 $(PKG_BUILD_DIR)/libreac.so
endef

# headers + lib into the SDK staging dir, so dependent packages can build against it
define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/reac
	$(CP) ./include/reac/reac.h $(1)/usr/include/reac/reac.h
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
