include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-freifunk-defaults
PKG_SOURCE_DATE:=2019-05-25
PKG_VERSION:=1

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-freifunk-defaults
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=Freifunk Berlin freifunk default configuration
  URL:=http://github.com/freifunk-berlin/packages_berlin
  PKGARCH:=all
endef

define Package/falter-berlin-freifunk-defaults/description
  Freifunk Berlin configuration files for freifunk
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-freifunk-defaults/install
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(CP) ./uci-defaults/* $(1)/etc/uci-defaults
endef

$(eval $(call BuildPackage,falter-berlin-freifunk-defaults))
