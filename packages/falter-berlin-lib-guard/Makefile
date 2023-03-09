include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-lib-guard
PKG_VERSION:=1

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-lib-guard
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=Freifunk Berlin guard function
  URL:=https://github.com/freifunk-berlin/falter-packages
  PKGARCH:=all
endef

define Package/falter-berlin-lib-guard/description
  Freifunk Berlin guard function
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-lib-guard/install
	$(CP) ./files/* $(1)/
endef

$(eval $(call BuildPackage,falter-berlin-lib-guard))
