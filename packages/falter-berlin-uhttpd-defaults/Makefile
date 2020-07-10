include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-uhttpd-defaults
PKG_VERSION:=1

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-uhttpd-defaults
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=Freifunk Berlin uhttpd default configuration
  URL:=http://github.com/Freifunk-Spalter/packages
  EXTRA_DEPENDS:=uhttpd, falter-berlin-lib-guard
  PKGARCH:=all
endef

define Package/falter-berlin-uhttpd-defaults/description
  Freifunk Berlin configuration files for uhttpd
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-uhttpd-defaults/install
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(CP) ./uci-defaults/* $(1)/etc/uci-defaults
endef

$(eval $(call BuildPackage,falter-berlin-uhttpd-defaults))
