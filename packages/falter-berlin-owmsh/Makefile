include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-owmsh
PKG_VERSION:=1.0

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-owmsh/default
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  URL:=https://github.com/freifunk-berlin/falter-packages
  PKGARCH:=all
endef

define Package/falter-berlin-owmsh
	$(call Package/falter-berlin-owmsh/default)
	TITLE:=Freifunk Berlin owm.sh
	EXTRA_DEPENDS:=uci jshn
endef


define Package/falter-berlin-owmsh/description
  Script for publishing a nodes position on the Freifunk Berlin network map
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-owmsh/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/owm.sh $(1)/usr/sbin/owm.sh
endef

$(eval $(call BuildPackage,falter-berlin-owmsh))
