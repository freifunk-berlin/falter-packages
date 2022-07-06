include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-firewall-defaults
PKG_VERSION:=3

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-firewall-defaults
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=Freifunk Berlin firewall default configuration
  URL:=http://github.com/freifunk-berlin/packages_berlin
  EXTRA_DEPENDS:=firewall4, falter-berlin-lib-guard
  PKGARCH:=all
endef

define Package/falter-berlin-firewall-defaults/description
  Freifunk Berlin configuration files for firewall
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-firewall-defaults/install
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(CP) ./uci-defaults/* $(1)/etc/uci-defaults
endef

$(eval $(call BuildPackage,falter-berlin-firewall-defaults))
