include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-uplink-tunnelberlin
PKG_VERSION:=3

include $(INCLUDE_DIR)/package.mk

Build/Compile=

define Package/falter-berlin-uplink-tunnelberlin
	SECTION:=falter-berlin
	CATEGORY:=falter-berlin
	TITLE:=Freifunk Berlin Networktunnel files
	URL:=http://github.com/freifunk-berlin/packages_berlin
	EXTRA_DEPENDS:=falter-berlin-lib-guard, falter-berlin-tunneldigger, falter-berlin-network-defaults
	PROVIDES:=falter-berlin-uplink
	PKGARCH:=all
endef

define Package/falter-berlin-uplink-tunnelberlin/description
  Freifunk Berlin files to setup a tunnel to route all upstream
  traffic to these community-internet-gateways.
endef

define Package/falter-berlin-uplink-tunnelberlin/install
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(CP) ./uci-defaults/freifunk-berlin-z95_tunnelberlin-tunneldigger $(1)/etc/uci-defaults
endef

$(eval $(call BuildPackage,falter-berlin-uplink-tunnelberlin))
