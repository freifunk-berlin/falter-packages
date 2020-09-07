include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-autoupdate
PKG_VERSION:=0.7.2
PKG_RELEASE:=1

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-autoupdate/default
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  PKGARCH:=all
endef

define Package/falter-berlin-autoupdate
  $(call Package/falter-berlin-autoupdate/default)
  TITLE:=Freifunk Berlin Autoupdate
  DEPENDS:=+uci +ntpclient
endef

define Package/falter-berlin-autoupdate/description
  autoupdate wants to get the upgrade process of a freifunk-berlin router via terminal smooth and easy.
endef

define Build/Prepare
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-autoupdate/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) ./files/autoupdate $(1)/usr/bin/autoupdate
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_DATA) ./files/config/config_autoupdate $(1)/etc/config/autoupdate
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/config/config_defaults.sh $(1)/etc/uci-defaults/freifunk-berlin-autoupdate.sh
	$(INSTALL_DIR) $(1)/usr/share/autoupdate/lib
	$(INSTALL_DATA) ./files/lib/urlencode.sed $(1)/usr/share/autoupdate/lib/urlencode.sed
	$(INSTALL_DATA) ./files/lib/libautoupdate.sh $(1)/usr/share/autoupdate/lib/libautoupdate.sh
	$(INSTALL_DIR) $(1)/usr/share/autoupdate/keys
	$(CP) ./files/keys/akira25.pub $(1)/usr/share/autoupdate/keys/akira25.pub
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/controller/admin
	$(INSTALL_BIN) ./files/LuCi/controller_autoupdate.lua $(1)/usr/lib/lua/luci/controller/admin/autoupdate.lua
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/model/cbi/autoupdate
	$(INSTALL_BIN) ./files/LuCi/cbi_autoupdate.lua $(1)/usr/lib/lua/luci/model/cbi/autoupdate/autoupdate.lua
endef

$(eval $(call BuildPackage,falter-berlin-autoupdate))
