# Copyright (C) 2011 Manuel Munz <freifunk at somakoma de>
# This is free software, licensed under the Apache 2.0 license.

include $(TOPDIR)/rules.mk

PKG_NAME:=falter-profiles
PKG_RELEASE:=2

PKG_BUILD_DIR := $(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/falter-profiles
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  SUBMENU:=9. Freifunk
  TITLE:=Falter profiles in json format
  PKGARCH:=all
endef

define Package/falter-profiles/description
  These falter profiles set defaults for various free network/freifunk communities and are used by the wizard-backend to configure the system.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-profiles/install
	$(INSTALL_DIR) $(1)/etc/freifunk/profiles
	$(INSTALL_DATA) ./files/etc/freifunk/profiles/* $(1)/etc/freifunk/profiles
endef

$(eval $(call BuildPackage,falter-profiles))
