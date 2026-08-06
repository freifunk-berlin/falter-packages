# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2026 Freifunk Berlin <info@berlin.freifunk.net>
#

include $(TOPDIR)/rules.mk

PKG_NAME:=falter-base
PKG_RELEASE:=0

PKG_MAINTAINER:=Freifunk Berlin <info@berlin.freifunk.net>
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

Build/Compile=

define Package/falter-base
	SECTION:=net
	CATEGORY:=Network
	TITLE:=falter-base
	URL:=https://github.com/freifunk-berlin/falter-packages
	PKGARCH:=all
	EXTRA_DEPENDS:= \
		ucode (>=0), ucode-mod-ubus (>=0), ucode-mod-fs (>=0), \
		rpcd (>=0), rpcd-mod-ucode (>=0)
endef

define Package/falter-base/install
	$(CP) ./root/* $(1)/
endef

$(eval $(call BuildPackage,falter-base))
