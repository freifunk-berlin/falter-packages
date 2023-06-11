#
# Copyright (C) 2012-2013 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-falter-owm
PKG_VERSION:=2022.08.29

PKG_BUILD_DIR := $(BUILD_DIR)/$(PKG_NAME)
PKG_BUILD_DEPENDS += lua/host luci-base/host LUCI_CSSTIDY:csstidy/host LUCI_SRCDIET:luasrcdiet/host $(LUCI_BUILD_DEPENDS)

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-falter-owm/default
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  URL:=https://github.com/freifunk-berlin/falter-packages
  PKGARCH:=all
endef

define Package/luci-app-falter-owm
  $(call Package/luci-app-falter-owm/default)
  EXTRA_DEPENDS:=luci-lib-json, olsrd-mod-jsoninfo, luci-lib-ip
  TITLE:=Luci JSON Export for Open Wireless Map
endef

define Package/luci-app-falter-owm/description
  Luci JSON Export for Open Wireless Map
endef

define Package/luci-app-falter-owm-cmd
  $(call Package/luci-app-falter-owm/default)
  EXTRA_DEPENDS:=luci-app-falter-owm, luci-lib-httpclient
  TITLE:=luci-app-falter-owm-cmd - Commandline update tool
endef

define Package/luci-app-falter-owm-gui
  $(call Package/luci-app-falter-owm/default)
  EXTRA_DEPENDS:=luci-app-falter-owm, luci-base
  TITLE:=luci-app-falter-owm-gui - GUI Open Wireless Map
endef

define Package/luci-app-falter-owm-ant
  $(call Package/luci-app-falter-owm/default)
  EXTRA_DEPENDS:=luci-app-falter-owm, luci-base
  TITLE:=luci-app-falter-owm-ant - GUI Antenna settings
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./luasrc $(PKG_BUILD_DIR)/
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/luci-app-falter-owm-cmd/postinst
#!/bin/sh
if [ -z $${IPKG_INSTROOT} ] ; then
	( . /etc/uci-defaults/owm ) && rm -f /etc/uci-defaults/owm
	rm -f /tmp/luci-indexcache
fi
endef

define Package/luci-app-falter-owm-cmd/install
	$(INSTALL_DIR) $(1)/usr/sbin/
	$(CP) files/owm.sh $(1)/usr/sbin/owm.sh
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(CP) files/owm-defaults $(1)/etc/uci-defaults/owm
endef

define Package/luci-app-falter-owm-gui/install
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/controller
	$(CP) $(PKG_BUILD_DIR)/luasrc/controller/owm.lua $(1)/usr/lib/lua/luci/controller/owm.lua
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/view
	$(CP) $(PKG_BUILD_DIR)/luasrc/view/owm.htm $(1)/usr/lib/lua/luci/view/owm.htm
endef

define Package/luci-app-falter-owm-ant/install
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/controller
	$(CP) $(PKG_BUILD_DIR)/luasrc/controller/antennas.lua $(1)/usr/lib/lua/luci/controller/antennas.lua
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/model/cbi
	$(CP) $(PKG_BUILD_DIR)/luasrc/model/cbi/antennas.lua $(1)/usr/lib/lua/luci/model/cbi/antennas.lua
endef

define Package/luci-app-falter-owm/install
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci
	$(CP) $(PKG_BUILD_DIR)/luasrc/owm.lua $(1)/usr/lib/lua/luci/owm.lua
endef

$(eval $(call BuildPackage,luci-app-falter-owm))
$(eval $(call BuildPackage,luci-app-falter-owm-cmd))
$(eval $(call BuildPackage,luci-app-falter-owm-gui))
$(eval $(call BuildPackage,luci-app-falter-owm-ant))
