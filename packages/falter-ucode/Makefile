# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2026 berlin.freifunk.net
#

include $(TOPDIR)/rules.mk

PKG_NAME:=falter-ucode
PKG_VERSION:=1.0
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0-only


include $(INCLUDE_DIR)/package.mk

Build/Compile=

define Package/falter-ucode/default
	SECTION:=falter-berlin
	CATEGORY:=falter-berlin
	TITLE:=ucode modules for freifunk berlin
	URL:=https://github.com/freifunk-berlin/falter-packages
	PKGARCH:=all
endef

define Package/falter-ucode
	$(Package/falter-ucode/default)
	EXTRA_DEPENDS:= ucode (>=0), ucode-mod-fs (>=0), ucode-mod-math (>=0)
endef

define Package/falter-ucode/install
	$(INSTALL_DIR) $(1)/usr/share/ucode/freifunk
	$(INSTALL_DATA) ./files/usr/share/ucode/freifunk/* $(1)/usr/share/ucode/freifunk
endef

$(eval $(call BuildPackage,falter-ucode))
