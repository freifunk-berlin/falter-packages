include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-gw
PKG_RELEASE:=0

PKG_BUILD_DIR := $(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-gw
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=Freifunk Gateway Node tools
  EXTRA_DEPENDS:=wireguard-tools (>=1), ubus (>=0)
  PKGARCH:=all
endef

define Package/falter-berlin-gw/description
  A set of tools used by the -gw nodes.  Monitors UBUS for a restart of OLSRd and re-adds all existing wireguard tunnels to OLSRd's mesh interfaces.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-gw/install
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/* $(1)/etc/init.d
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/usr/sbin/* $(1)/usr/sbin
endef

$(eval $(call BuildPackage,falter-berlin-gw))
