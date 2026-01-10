# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2024 Simon Polack <spolack+git@mailbox.org>
#

include $(TOPDIR)/rules.mk

PKG_NAME:=bgpdisco
PKG_VERSION:=1.1
PKG_RELEASE:=1

PKG_MAINTAINER:=Simon Polack <spolack+git@mailbox.org>
PKG_LICENSE:=GPL-2.0-only


include $(INCLUDE_DIR)/package.mk

Build/Compile=

define Package/bgpdisco/default
	SECTION:=net
	CATEGORY:=Network
	TITLE:=BGP node discovery agent
	URL:=https://github.com/freifunk-berlin/falter-packages
	PKGARCH:=all
endef

define Package/bgpdisco
	$(Package/bgpdisco/default)
	EXTRA_DEPENDS:= \
		ucode (>=0), ucode-mod-rtnl (>=0), ucode-mod-debug (>=0), ucode-mod-fs (>=0), \
		ucode-mod-uloop (>=0), ucode-mod-log (>=0), ucode-mod-struct (>=0)
endef

define Package/bgpdisco-plugin-nameservice
	$(Package/bgpdisco/default)
	TITLE+= - Nameservice Plugin
	EXTRA_DEPENDS:=bgpdisco (=$(PKG_VERSION)-r$(PKG_RELEASE))
endef


define Package/bgpdisco/conffiles
/etc/config/bgpdisco
endef

define Package/bgpdisco-plugin-nameservice/conffiles
/etc/config/bgpdisco_nameservice
endef


define Package/bgpdisco/install
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./bgpdisco.init $(1)/etc/init.d/bgpdisco

	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./bgpdisco.conf $(1)/etc/config/bgpdisco

	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) ./bgpdisco.uc $(1)/usr/bin/bgpdisco

	$(INSTALL_DIR) $(1)/usr/share/ucode/bgpdisco
	$(INSTALL_BIN) ./birdctl.uc $(1)/usr/share/ucode/bgpdisco/birdctl.uc
	$(INSTALL_BIN) ./plugin.uc $(1)/usr/share/ucode/bgpdisco/plugin.uc
	$(INSTALL_BIN) ./logger.uc $(1)/usr/share/ucode/bgpdisco/logger.uc
	$(INSTALL_BIN) ./mrtdump.uc $(1)/usr/share/ucode/bgpdisco/mrtdump.uc
	$(INSTALL_BIN) ./bird_config_template.ut $(1)/usr/share/ucode/bgpdisco/bird_config_template.ut

	$(INSTALL_DIR) $(1)/usr/share/ucode/bgpdisco/plugins
endef

define Package/bgpdisco-plugin-nameservice/install
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./bgpdisco_nameservice.conf $(1)/etc/config/bgpdisco_nameservice

	$(INSTALL_DIR) $(1)/usr/share/ucode/bgpdisco/plugins
	$(INSTALL_BIN) ./nameservice.uc $(1)/usr/share/ucode/bgpdisco/plugins/nameservice.uc
endef

$(eval $(call BuildPackage,bgpdisco))
$(eval $(call BuildPackage,bgpdisco-plugin-nameservice))
