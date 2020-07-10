
include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-uplink-tunnelberlin-files
PKG_VERSION:=1

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-uplink-tunnelberlin-files/Default
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=Freifunk Berlin Networktunnel files
  URL:=http://github.com/freifunk-berlin/packages_berlin
  EXTRA_DEPENDS:=falter-berlin-lib-guard
  PROVIDES:=falter-berlin-uplink
  PKGARCH:=all
endef

define Package/falter-berlin-uplink-tunnelberlin-tunneldigger
$(call Package/falter-berlin-uplink-tunnelberlin-files/Default)
  TITLE+= (Tunneldigger)
  VARIANT:=tunneldigger
  EXTRA_DEPENDS:=falter-berlin-tunneldigger, falter-berlin-network-defaults
endef

define Package/falter-berlin-uplink-tunnelberlin-files/description
  Freifunk Berlin files to setup a tunnel to route all
  upstream traffic to these community-internet-gateways
endef

define Package/falter-berlin-uplink-tunnelberlin-tunneldigger/description
$(call Package/falter-berlin-uplink-tunnelberlin-files/description)
  This variant uses the tunneldigger-protocol.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-uplink-tunnelberlin-tunneldigger/install
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(CP) ./uci-defaults/freifunk-berlin-z95_tunnelberlin-tunneldigger $(1)/etc/uci-defaults
endef

$(eval $(call BuildPackage,falter-berlin-uplink-tunnelberlin-tunneldigger))
