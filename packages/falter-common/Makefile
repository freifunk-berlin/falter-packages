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
  EXTRA_DEPENDS:=uci
  PKGARCH:=all
endef

define Package/falter-common/description
  Common files and scripts that are needed to run free wireless mesh networks.
endef

define Package/falter-common/conffiles
/etc/config/freifunk
endef

define Package/falter-common-olsr
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=9. Freifunk
  TITLE:=Falter common files for olsr-v1
  EXTRA_DEPENDS:=falter-common, olsrd, libuci-lua, lua
  PKGARCH:=all
endef

define Package/falter-common-olsr/description
  Additional files and scripts that are needed to run free wireless mesh networks based on OLSRv1.
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
	$(CP) ./files-common/* $(1)/
	$(SED) 's,%R,$(FALTER_REVISION),g' $(1)/etc/freifunk_release
endef

define Package/falter-common-olsr/install
	$(CP) ./files-olsrv1/* $(1)/
endef

$(eval $(call BuildPackage,falter-common))
$(eval $(call BuildPackage,falter-common-olsr))
