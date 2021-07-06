include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-ssid-changer
PKG_VERSION:=1

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-ssid-changer
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=Freifunk Berlin ssid changer
  URL:=http://github.com/Freifunk-Spalter/packages
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
	$(INSTALL_DIR) $(1)/etc/pingcheck
	$(CP) ./files/online.d $(1)/etc/pingcheck/online.d
	$(CP) ./files/offline.d $(1)/etc/pingcheck/offline.d
	$(CP) ./files/lib $(1)/lib
endef

$(eval $(call BuildPackage,falter-berlin-ssid-changer))
