include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-admin-keys
PKG_VERSION:=2

include $(INCLUDE_DIR)/package.mk

define Package/falter-berlin-admin-keys
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=Freifunk Berlin Administrator SSH Keys
  URL:=http://github.com/Freifunk-Spalter/packages
  PKGARCH:=all
endef

define Package/falter-berlin-admin-keys/description
  This package contains the ssh-keys of Freifunkers volunteering in backbone administratrion.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-berlin-admin-keys/install
	$(INSTALL_DIR) $(1)/tmp/falter-berlin-admin-keys
	$(CP) ./files/* $(1)/tmp/falter-berlin-admin-keys
endef

define Package/falter-berlin-admin-keys/postinst
#!/bin/sh
$${IPKG_INSTROOT}/tmp/falter-berlin-admin-keys/register_keys.sh
endef

$(eval $(call BuildPackage,falter-berlin-admin-keys))
