# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2026 Freifunk Berlin <info@berlin.freifunk.net>
#

include $(TOPDIR)/rules.mk

PKG_NAME:=falter-web
PKG_RELEASE:=0

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
		luci-base (>=0), luci-theme-bootstrap (>=0), \
		px5g-mbedtls (>=0), uhttpd (>=0), uhttpd-mod-ubus (>=0), uhttpd-mod-ucode (>=0)
endef

define Package/falter-web/install
	$(CP) ./root/* $(1)/
endef

$(eval $(call BuildPackage,falter-web))
