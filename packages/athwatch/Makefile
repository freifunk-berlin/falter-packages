# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2022 Packet Please <pktpls@systemli.org>
#

include $(TOPDIR)/rules.mk

PKG_NAME:=athwatch
PKG_VERSION:=1
PKG_RELEASE:=$(AUTORELEASE)

PKG_MAINTAINER:=Packet Please <pktpls@systemli.org>
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/athwatch
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Watchdog for ath10k wifi bugs
  PKGARCH:=all
endef

define Package/athwatch/description
Reboots when a certain ath10k_pci bug is detected.
endef

define Build/Compile
endef

define Package/athwatch/install
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/athwatch.init $(1)/etc/init.d/athwatch
endef

$(eval $(call BuildPackage,athwatch))
