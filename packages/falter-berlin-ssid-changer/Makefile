include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-ssid-changer
PKG_VERSION:=2

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-ssid-changer
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=Freifunk Berlin SSID Changer
  URL:=https://github.com/freifunk-berlin/falter-packages
  PKGARCH:=all
  EXTRA_DEPENDS:=pingcheck
endef

define Package/falter-berlin-ssid-changer/description
  Scripts for changing the SSID of Freifunk-Routers when they go offline.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-ssid-changer/install
	$(INSTALL_DIR) $(1)/etc/hotplug.d/net
	$(CP) ./files/hotplug.d/* $(1)/etc/hotplug.d/net
	$(CP) ./files/lib $(1)/lib
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(CP) ./files/post-inst.sh $(1)/etc/uci-defaults/90_ssidchanger-post-inst.sh
endef

$(eval $(call BuildPackage,falter-berlin-ssid-changer))
