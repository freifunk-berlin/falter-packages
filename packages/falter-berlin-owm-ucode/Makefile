# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2026 Tobias Schwarz <info@tobias-schwarz.com>
#

include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-owm-ucode
PKG_VERSION:=1.0

PKG_MAINTAINER:=Tobias Schwarz <info@tobias-schwarz.com>
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-owm-ucode/default
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  URL:=https://github.com/freifunk-berlin/falter-packages
  PKGARCH:=all
endef

define Package/falter-berlin-owm-ucode
	$(call Package/falter-berlin-owm-ucode/default)
	TITLE:=Freifunk Berlin owm.ucode
  DEPENDS:=+ucode +ucode-mod-fs +ucode-mod-ubus +ucode-mod-uci +ucode-mod-resolv +ucode-mod-uloop +uclient-fetch
endef

define Package/falter-berlin-owm-ucode/description
  ucode reimplementation of owm.sh for publishing a nodes position on the Freifunk Berlin network map
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-owm-ucode/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) ./owm.uc $(1)/usr/bin/owm
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./owm.init $(1)/etc/init.d/owm
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./owm.defaults $(1)/etc/uci-defaults/owm
endef

$(eval $(call BuildPackage,falter-berlin-owm-ucode))
