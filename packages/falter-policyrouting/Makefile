# Copyright (C) 2011 Manuel Munz <freifunk at somakoma de>
# This is free software, licensed under the Apache 2.0 license.

include $(TOPDIR)/rules.mk

PKG_NAME:=falter-policyrouting
PKG_RELEASE:=3

PKG_BUILD_DIR := $(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/falter-policyrouting
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=9. Freifunk
  TITLE:=Freifunk policy routing addon
  EXTRA_DEPENDS:=ip
  PKGARCH:=all
endef

define Package/falter-policyrouting/description
  Allows you to send your own traffic via your own default gateway while sending traffic received from the mesh to a gateway in the mesh.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-policyrouting/install
	$(CP) ./files/* $(1)/
endef

$(eval $(call BuildPackage,falter-policyrouting))
