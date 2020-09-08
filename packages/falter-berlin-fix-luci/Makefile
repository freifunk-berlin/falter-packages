include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-fix-luci
PKG_VERSION:=1

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-fix-luci
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=Freifunk Berlin fix-luci
  URL:=http://github.com/Freifunk-Spalter/packages
  PKGARCH:=all
endef

define Package/falter-berlin-fix-luci/description
  Freifunk Berlin uci-defaults-script for fixing luci-button-not-shown-bug.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-fix-luci/install
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(CP) ./files/* $(1)/etc/uci-defaults
endef

$(eval $(call BuildPackage,falter-berlin-fix-luci))
