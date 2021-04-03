#
# Copyright (C) 2016 Freifunk Berlin
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=falter-airos-common
PKG_VERSION:=0.0.1
PKG_RELEASE:=1

UBIQUITI_MIBS:=https://www.ui.com/downloads/firmwares/airos-ubnt-mib/ubnt-mib.zip

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/falter-airos-common/default
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  URL:=http://github.com/Freifunk-Spalter/packages
  PKGARCH:=all
endef

define Package/falter-airos-snmp-mibs
  $(call Package/falter-airos-common/default)
  TITLE:=Ubiquiti AirOS SNMP MIBS
  EXTRA_DEPENDS:=snmp-mibs
endef

define Package/falter-airos-snmp-mibs/description
  Ubiquiti AirOS SNMP mibs fetched from UBNT Website
endef

define Package/falter-airos-dfs-reset
  $(call Package/collectd-addons/default)
  TITLE:=AirOS DFS Reset
  DEPENDS:=+luci-app-statistics +collectd-mod-exec dnsmasq
endef

define Package/falter-airos-dfs-reset/description
  tool to revert channel change after DFS event
endef

define Download/ubnt_mibs
	URL_FILE:=ubnt-mib.zip
	URL:=https://www.ubnt.com/downloads/firmwares/airos-ubnt-mib/
	MD5SUM:=7e7dff89b99e51336c09c4e4dad53287
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	mkdir -p $(PKG_BUILD_DIR)/ubnt-mibs
	unzip -j $(DL_DIR)/ubnt-mib.zip -d $(PKG_BUILD_DIR)/ubnt-mibs
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-airos-snmp-mibs/install
	$(INSTALL_DIR) $(1)/usr/share/snmp/mibs
	$(CP) $(PKG_BUILD_DIR)/ubnt-mibs/* $(1)/usr/share/snmp/mibs
endef

define Package/falter-airos-dfs-reset/install
	$(INSTALL_DIR) $(1)/usr/bin/
	$(INSTALL_BIN) ./files/falter-airos-dfs-reset/airos-dfs-reset $(1)/usr/bin/
	$(INSTALL_DIR) $(1)/etc/config/
	$(INSTALL_BIN) ./files/falter-airos-dfs-reset/config $(1)/etc/config/airos-dfs-reset
endef

$(eval $(call Download,ubnt_mibs))
$(eval $(call BuildPackage,falter-airos-snmp-mibs))
$(eval $(call BuildPackage,falter-airos-dfs-reset))
