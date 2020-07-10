
include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-uplink-notunnel-files
PKG_VERSION:=1

include $(INCLUDE_DIR)/package.mk

define Package/$(PKG_NAME)
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=Freifunk Berlin no tunnel files
  URL:=http://github.com/freifunk-berlin/packages_berlin
  EXTRA_DEPENDS:=falter-berlin-lib-guard, kmod-veth, falter-berlin-network-defaults, pingcheck
  PROVIDES:=falter-berlin-uplink
  PKGARCH:=all
endef

define Package/$(PKG_NAME)/description
  Freifunk Berlin files to setup a uplink without any
  tunnel.
  upstream traffic will be directly routed via your network
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/$(PKG_NAME)/install
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(CP) ./uci-defaults/* $(1)/etc/uci-defaults
	$(INSTALL_DIR) $(1)/etc/pingcheck/online.d
	$(CP) ./files/etc/pingcheck/online.d/* $(1)/etc/pingcheck/online.d
	$(INSTALL_DIR) $(1)/etc/pingcheck/offline.d
	$(CP) ./files/etc/pingcheck/offline.d/* $(1)/etc/pingcheck/offline.d
endef

$(eval $(call BuildPackage,$(PKG_NAME)))
