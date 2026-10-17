# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2026 berlin.freifunk.net
#

include $(TOPDIR)/rules.mk

PKG_NAME:=falter-wizard-backend
PKG_VERSION:=1.0
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0-only


include $(INCLUDE_DIR)/package.mk

Build/Compile=

define Package/falter-wizard-backend/default
	SECTION:=falter-berlin
	CATEGORY:=falter-berlin
	TITLE:=backend of the freifunk wizard to configure a router and start services
	URL:=https://github.com/freifunk-berlin/falter-packages
	PKGARCH:=all
endef

define Package/falter-wizard-backend
	$(Package/falter-wizard-backend/default)
	EXTRA_DEPENDS:= ucode (>=0), ucode-mod-fs (>=0), ucode-mod-ubus (>=0)
endef

define Package/falter-wizard-backend/install
	$(INSTALL_DIR) $(1)/usr/share/ucode/freifunk/wizard/config
	$(INSTALL_DATA) ./files/usr/share/ucode/freifunk/wizard/config/* $(1)/usr/share/ucode/freifunk/wizard/config
	$(INSTALL_DIR) $(1)/usr/share/ucode/freifunk/wizard/services
	$(INSTALL_DATA) ./files/usr/share/ucode/freifunk/wizard/services/* $(1)/usr/share/ucode/freifunk/wizard/services
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/usr/sbin/wizard-backend $(1)/usr/sbin/wizard-backend
endef

$(eval $(call BuildPackage,falter-wizard-backend))
