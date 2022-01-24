include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-autoupdate
PKG_VERSION:=0.1

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-autoupdate/default
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  URL:=http://github.com/Freifunk-Spalter/packages
  PKGARCH:=all
endef

define Package/falter-berlin-autoupdate
	$(call Package/falter-berlin-autoupdate/default)
	TITLE:=Freifunk Berlin Autoupdater
	EXTRA_DEPENDS:=falter-berlin-autoupdate-keys uci usign
endef

define Package/falter-berlin-autoupdate-keys
	$(call Package/falter-berlin-autoupdate/default)
	TITLE:=Freifunk Berlin Autoupdater - Keys
endef

define Package/falter-berlin-autoupdate/description
  Script for managing autoupdates on Freifunk Berlin routers
endef

define Package/falter-berlin-autoupdate-keys/description
  Script for managing autoupdates on Freifunk Berlin routers
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-autoupdate/conffiles
/etc/config/autoupdate
endef

define Package/falter-berlin-autoupdate/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) ./files/autoupdate.sh $(1)/usr/bin/autoupdate
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/autoupdate.conf $(1)/etc/config/autoupdate
	$(INSTALL_DIR) $(1)/lib/autoupdate
	$(CP) ./files/lib_autoupdate.sh $(1)/lib/autoupdate/lib_autoupdate.sh
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(CP) ./files/post-inst.sh $(1)/etc/uci-defaults/90_autoupdate-post-inst.sh
endef

define Package/falter-berlin-autoupdate-keys/install
	$(INSTALL_DIR) $(1)/etc/autoupdate/keys
	$(CP) ./keys/* $(1)/etc/autoupdate/keys/
endef

$(eval $(call BuildPackage,falter-berlin-autoupdate))
$(eval $(call BuildPackage,falter-berlin-autoupdate-keys))
