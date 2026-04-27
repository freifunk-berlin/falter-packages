include $(TOPDIR)/rules.mk

PKG_NAME:=falter-bird-utils
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_MAINTAINER:=Aditya Chooramani - adityaxchooramani@gmail.com

include $(INCLUDE_DIR)/package.mk

define Package/falter-bird-utils
  SECTION:=net
  CATEGORY:=Network
  TITLE:=UCI-to-Bird2 Translator for Falter
  DEPENDS:=+bird2 +ip-full
endef

define Package/falter-bird-utils/description
  Automates Bird2 configuration for Falter Mesh Nodes using UCI.
endef

define Build/Compile
endef

define Package/falter-bird-utils/install
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/bird.init $(1)/etc/init.d/bird
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_DATA) ./files/bird.config $(1)/etc/config/bird
endef

$(eval $(call BuildPackage,falter-bird-utils))