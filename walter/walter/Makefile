# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2026 Freifunk Berlin <info@berlin.freifunk.net>
#

include $(TOPDIR)/rules.mk

PKG_NAME:=walter
PKG_RELEASE:=1

PKG_MAINTAINER:=Freifunk Berlin <info@berlin.freifunk.net>
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/walter/default
	SECTION:=net
	CATEGORY:=Network
	TITLE:=walter
	URL:=https://github.com/freifunk-berlin/falter-packages
	PKGARCH:=all
endef

define Package/walter-full
	$(Package/walter/default)
	PROVIDES:=walter
	VARIANT:=full
	DEPENDS:=+walter-base +walter-web +walter-tunnel +walter-stats +walter-legacy +walter-tools
endef

define Package/walter-mini
	$(Package/walter/default)
	TITLE+= (minimal version)
	PROVIDES:=walter
	VARIANT:=mini
	DEPENDS:=+walter-base
endef

define Package/walter-tools
	SECTION:=net
	CATEGORY:=Network
	TITLE:=walter-tools
	URL:=https://github.com/freifunk-berlin/falter-packages
	PKGARCH:=all
	DEPENDS:=+tcpdump +mtr +iperf3 +tmux
endef

Build/Compile = true
Package/walter-full/install = true
Package/walter-mini/install = true

$(eval $(call BuildPackage,walter-full))
$(eval $(call BuildPackage,walter-mini))
