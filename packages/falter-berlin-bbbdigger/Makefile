#
# Copyright (C) 2016 Freifunk Berlin
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-bbbdigger
PKG_VERSION:=0.0.3
PKG_RELEASE:=3

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-bbbdigger/default
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  URL:=https://github.com/freifunk-berlin/falter-packages
  PKGARCH:=all
endef

define Package/falter-berlin-bbbdigger
  $(call Package/falter-berlin-bbbdigger/default)
  TITLE:=A Tunneldigger (l2tp) based VPN connection to mesh with the Berlin Backbone
  EXTRA_DEPENDS:=falter-berlin-tunneldigger, olsrd
endef

define Package/falter-berlin-bbbdigger/description
  Freifunk Berlin configuration of tunneldigger to connect to and mesh with the Berlin Backbone
endef

define Build/Prepare
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-bbbdigger/install
  $(CP) ./files/* $(1)/
endef

$(eval $(call BuildPackage,falter-berlin-bbbdigger))
