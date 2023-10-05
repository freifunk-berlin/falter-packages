include $(TOPDIR)/rules.mk

PKG_NAME:=tunspace
PKG_RELEASE:=0

PKG_MAINTAINER:=Packet Please <pktpls@systemli.org>

include $(INCLUDE_DIR)/package.mk

Build/Compile=

define Package/tunspace
	SECTION:=net
	CATEGORY:=Network
	TITLE:=TunSpace
	URL:=https://github.com/freifunk-berlin/falter-packages
	PKGARCH:=all
	EXTRA_DEPENDS:= \
		ucode, ucode-mod-wireguard, ucode-mod-rtnl, \
		ucode-mod-uloop, ucode-mod-fs, ucode-mod-math, \
		kmod-macvlan, uclient-fetch
endef

define Package/tunspace/conffiles
/etc/config/tunspace
endef

define Package/tunspace/install
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_DIR) $(1)/usr/bin

	$(INSTALL_BIN) ./tunspace.init $(1)/etc/init.d/tunspace
	$(INSTALL_BIN) ./tunspace.defaults $(1)/etc/uci-defaults/tunspace
	$(INSTALL_BIN) ./tunspace.uc $(1)/usr/bin/tunspace
endef

define Package/tunspace/conffiles
/etc/config/tunspace
endef

$(eval $(call BuildPackage,tunspace))
