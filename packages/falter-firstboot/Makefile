# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2026 berlin.freifunk.net
#

include $(TOPDIR)/rules.mk

PKG_NAME:=falter-firstboot
PKG_VERSION:=1.0
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0-only


include $(INCLUDE_DIR)/package.mk

Build/Compile=

define Package/falter-firstboot/default
	SECTION:=falter-berlin
	CATEGORY:=falter-berlin
	TITLE:=firstboot ucode modules for freifunk berlin
	URL:=https://github.com/freifunk-berlin/falter-packages
	PKGARCH:=all
endef

define Package/falter-firstboot
	$(Package/falter-firstboot/default)
	EXTRA_DEPENDS:= falter-ucode (>=0)
endef

define Package/falter-firstboot/install
	$(INSTALL_DIR) $(1)/etc/freifunk/firstboot
	$(INSTALL_DATA) ./files/etc/freifunk/firstboot/* $(1)/etc/freifunk/firstboot
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_DATA) ./files/etc/uci-defaults/* $(1)/etc/uci-defaults
endef

$(eval $(call BuildPackage,falter-firstboot))
