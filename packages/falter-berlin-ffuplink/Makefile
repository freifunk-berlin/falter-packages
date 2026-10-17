# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2026 berlin.freifunk.net
#

include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-ffuplink
PKG_VERSION:=1.0
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0-only


include $(INCLUDE_DIR)/package.mk

Build/Compile=

define Package/falter-berlin-ffuplink/default
	SECTION:=falter-berlin
	CATEGORY:=falter-berlin
	TITLE:=backend of the freifunk wizard to configure a router and start services
	URL:=https://github.com/freifunk-berlin/falter-packages
	PKGARCH:=all
endef

define Package/falter-berlin-ffuplink
	$(Package/falter-berlin-ffuplink/default)
	EXTRA_DEPENDS:= ucode (>=0), ucode-mod-fs (>=0), ucode-mod-ubus (>=0)
endef

define Package/falter-berlin-ffuplink/install
	$(INSTALL_DIR) $(1)/usr/share/ucode/freifunk/ffuplink
	$(INSTALL_DATA) ./files/usr/share/ucode/freifunk/ffuplink/* $(1)/usr/share/ucode/freifunk/ffuplink
endef

$(eval $(call BuildPackage,falter-berlin-ffuplink))
