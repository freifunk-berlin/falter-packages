include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-statistics-defaults
PKG_SOURCE_DATE:=2020-04-01
PKG_VERSION:=1

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-statistics-defaults
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=Freifunk Berlin statistics default configuration
  URL:=http://github.com/Freifunk-Spalter/packages
  EXTRA_DEPENDS:=olsrd-mod-txtinfo, falter-berlin-lib-guard
  PKGARCH:=all
endef

define Package/falter-berlin-statistics-defaults/description
  Freifunk Berlin configuration files for statistics
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-statistics-defaults/install
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(CP) ./uci-defaults/* $(1)/etc/uci-defaults
endef

$(eval $(call BuildPackage,falter-berlin-statistics-defaults))
