include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-network-defaults
PKG_VERSION:=5

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-network-defaults
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=Freifunk Berlin network default configuration
  URL:=http://github.com/Freifunk-Spalter/packages
  EXTRA_DEPENDS:=falter-berlin-lib-guard, pingcheck
  PKGARCH:=all
endef

define Package/falter-berlin-network-defaults/description
  Freifunk Berlin configuration files for network
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-network-defaults/install
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(CP) ./uci-defaults/* $(1)/etc/uci-defaults
	$(INSTALL_DIR) $(1)/etc/hotplug.d/iface
	$(CP) ./hotplug.d/iface/* $(1)/etc/hotplug.d/iface/
	$(INSTALL_DIR) $(1)/lib/functions
	$(CP) ./lib/functions/* $(1)/lib/functions/
	$(INSTALL_DIR) $(1)/etc/pingcheck/online.d
	$(CP) ./pingcheck/online.d/* $(1)/etc/pingcheck/online.d/
	$(INSTALL_DIR) $(1)/etc/pingcheck/offline.d
	$(CP) ./pingcheck/offline.d/* $(1)/etc/pingcheck/offline.d/
endef

$(eval $(call BuildPackage,falter-berlin-network-defaults))
