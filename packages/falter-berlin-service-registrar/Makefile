include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-service-registrar
PKG_VERSION:=0.2

PKG_LICENSE:=GPL-3.0-or-later
PKG_LICENSE_FILES:=LICENSE

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-service-registrar/default
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  URL:=https://github.com/freifunk-berlin/falter-packages
  PKGARCH:=all
  EXTRA_DEPENDS:=olsrd, olsrd-mod-nameservice, uhttpd, uci
endef

define Package/falter-berlin-service-registrar
	$(call Package/falter-berlin-service-registrar/default)
	TITLE:=Freifunk Berlin Service Registrar
endef


define Package/falter-berlin-service-registrar/description
  App for simplifying the registration and anouncment of services in Freifunk Berlin Mesh-Network.
endef


define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-service-registrar/conffiles
/etc/config/ffservices
endef

define Package/falter-berlin-service-registrar/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) ./files/register-services.sh $(1)/usr/bin/register-services
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/ffservices.conf $(1)/etc/config/ffservices
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/ffserviced.sh $(1)/etc/init.d/ffserviced
endef

$(eval $(call BuildPackage,falter-berlin-service-registrar))
