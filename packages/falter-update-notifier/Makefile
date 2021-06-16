include $(TOPDIR)/rules.mk

PKG_NAME:=falter-update-notifier
PKG_RELEASE:=1

PKG_BUILD_DIR := $(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/falter-update-notifier
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=9. Freifunk
  TITLE:=Falter Update Notifier
  EXTRA_DEPENDS:=falter-common
  PKGARCH:=all
endef

define Package/falter-update-notifier/description
  This contains a script that checks for firmware updates weekly. If there are updates, it shows a notification on login.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-update-notifier/install
	$(CP) ./files/* $(1)/
endef

$(eval $(call BuildPackage,falter-update-notifier))
