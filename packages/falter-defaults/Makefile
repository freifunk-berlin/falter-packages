
include $(TOPDIR)/rules.mk

PKG_NAME:=falter-defaults
PKG_RELEASE:=1

PKG_BUILD_DIR := $(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-defaults
	SECTION:=luci
	CATEGORY:=LuCI
	SUBMENU:=9. Freifunk
	URL:=https://github.com/freifunk-berlin/falter-packages
	PKGARCH:=all
	TITLE:=Falter default configuration
	EXTRA_DEPENDS:=falter-common
endef

define Package/falter-defaults/description
	UCI defaults scripts which bootstrap a Falter router.
endef

define Package/falter-defaults/conffiles
endef

define Package/falter-defaults/install
	$(CP) ./files/* $(1)/
endef

$(eval $(call BuildPackage,falter-defaults))
