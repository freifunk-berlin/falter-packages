include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-tunnelmanager
PKG_VERSION:=0.1

include $(INCLUDE_DIR)/package.mk

# todo: add dependency on jshn

define Package/falter-berlin-tunnelmanager
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=Freifunk Berlin tunnelmanager
  URL:=http://github.com/Freifunk-Spalter/packages
  EXTRA_DEPENDS:=ip-full, kmod-macvlan, wg-installer-client
  PKGARCH:=all
endef

define Package/falter-berlin-tunnelmanager/description
  Scripts for managing wireguard-tunnels on Freifunk Berlin routers.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-tunnelmanager/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(CP) ./files/tunnelman.sh $(1)/usr/bin/tunnelman
	$(INSTALL_DIR) $(1)/usr/share/tunnelman
	$(CP) ./files/up.sh $(1)/usr/share/tunnelman/up.sh
	$(CP) ./files/down.sh $(1)/usr/share/tunnelman/down.sh
endef

$(eval $(call BuildPackage,falter-berlin-tunnelmanager))
