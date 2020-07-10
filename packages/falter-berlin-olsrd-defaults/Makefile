include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-olsrd-defaults
PKG_VERSION:=1

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-olsrd-defaults
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=Freifunk Berlin olsrd default configuration
  URL:=http://github.com/Freifunk-Spalter/packages
  EXTRA_DEPENDS+=olsrd, falter-berlin-lib-guard
  PKGARCH:=all
endef

define Package/falter-berlin-olsrd-defaults/description
  Freifunk Berlin configuration files for olsrd and olsrd6
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-olsrd-defaults/install
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(CP) ./uci-defaults/* $(1)/etc/uci-defaults
endef

$(eval $(call BuildPackage,falter-berlin-olsrd-defaults))
