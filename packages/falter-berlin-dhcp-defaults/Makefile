include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-dhcp-defaults
PKG_VERSION:=1

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-dhcp-defaults
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=Freifunk Berlin dhcp default configuration
  URL:=http://github.com/freifunk-berlin/packages_berlin
  DEPENDS+= +dnsmasq +falter-berlin-lib-guard
  PKGARCH:=all
endef

define Package/falter-berlin-dhcp-defaults/description
  Freifunk Berlin configuration files for dhcp
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-dhcp-defaults/install
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(CP) ./uci-defaults/* $(1)/etc/uci-defaults
	$(CP) ./files/* $(1)/
endef

$(eval $(call BuildPackage,falter-berlin-dhcp-defaults))

