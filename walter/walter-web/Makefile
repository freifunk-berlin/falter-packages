# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2026 Freifunk Berlin <info@berlin.freifunk.net>
#

include $(TOPDIR)/rules.mk

PKG_NAME:=falter-web
PKG_RELEASE:=2

PKG_MAINTAINER:=Freifunk Berlin <info@berlin.freifunk.net>
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

Build/Compile=

define Package/falter-web
	SECTION:=net
	CATEGORY:=Network
	TITLE:=falter-web
	URL:=https://github.com/freifunk-berlin/falter-packages
	PKGARCH:=all
	EXTRA_DEPENDS:= \
		libuhttpd-mbedtls (>= 0), uhttpd-mod-ucode (>= 0), uhttpd-mod-ubus (>= 0)
endef

define Package/falter-web/install
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_DIR) $(1)/usr/bin

	$(INSTALL_BIN) ./tunspace.init $(1)/etc/init.d/tunspace
	$(INSTALL_BIN) ./tunspace.defaults $(1)/etc/uci-defaults/tunspace
	$(INSTALL_BIN) ./tunspace.uc $(1)/usr/bin/tunspace
endef

$(eval $(call BuildPackage,falter-web))
