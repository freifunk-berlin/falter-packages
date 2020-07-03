include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-migration
PKG_VERSION:=1

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-migration
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=Freifunk Berlin configuration migration script
  URL:=http://github.com/freifunk-berlin/packages_berlin
  DEPENDS+= +falter-berlin-lib-guard
  PKGARCH:=all
endef

define Package/falter-berlin-migration/description
  Freifunk Berlin configuration migration script
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-migration/install
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(CP) ./uci-defaults/* $(1)/etc/uci-defaults
	$(INSTALL_DIR) $(1)/lib/functions
	$(CP) ./files/* $(1)/
endef

$(eval $(call BuildPackage,falter-berlin-migration))

