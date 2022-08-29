include $(TOPDIR)/rules.mk

PKG_NAME:=falter-common
PKG_RELEASE:=4

PKG_BUILD_DIR := $(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/falter-common
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=9. Freifunk
  TITLE:=Falter common files
  EXTRA_DEPENDS:=uci, libuci-lua, lua, olsrd
  PKGARCH:=all
endef

define Package/falter-common/description
  Common files and scripts that are needed to run free wireless mesh networks.
endef

define Package/falter-common/conffiles
/etc/config/freifunk
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

FALTER_REVISION:=$(shell git -C $(TOPDIR)/feeds/falter describe --exact-match --tags 2> /dev/null || git -C $(TOPDIR)/feeds/falter rev-parse --short HEAD)

define Package/falter-common/install
	$(CP) ./files/* $(1)/
	$(SED) 's,%R,$(FALTER_REVISION),g' $(1)/etc/freifunk_release
endef

$(eval $(call BuildPackage,falter-common))
