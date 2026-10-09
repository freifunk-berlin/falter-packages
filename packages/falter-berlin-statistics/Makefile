# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2026 berlin.freifunk.net

include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-statistics
PKG_VERSION:=2028.10.09
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

Build/Compile=

define Package/falter-berlin-statistics/default
	SECTION:=falter-berlin
	CATEGORY:=falter-berlin
	TITLE:=Statistics package for Freifunk Falter using prometheus node exporter.
	SUBMENU:=9. Freifunk
	URL:=https://github.com/freifunk-berlin/falter-packages
	PKGARCH:=all
endef

define Package/falter-berlin-statistics
	$(Package/falter-berlin-statistics/default)
	EXTRA_DEPENDS:=prometheus-node-exporter-ucode (>=0),
	EXTRA_DEPENDS+= prometheus-node-exporter-ucode-openwrt (>=0),
	EXTRA_DEPENDS+= prometheus-node-exporter-ucode-wifi (>=0),
	EXTRA_DEPENDS+= ucode-mod-fs (>=0),
	EXTRA_DEPENDS+= ucode-mod-math (>=0),
	EXTRA_DEPENDS+= ucode-mod-ubus (>=0),
	EXTRA_DEPENDS+= ucode-mod-uci (>=0),
	EXTRA_DEPENDS+= wget-any (>=0)
endef

define Package/falter-berlin-statistics/conffiles
/etc/config/prometheus-push
endef

define Package/falter-berlin-statistics/description
	Statistics package for Freifunk Falter using prometheus node exporter.
endef

define Package/falter-berlin-statistics/install
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/prometheus-push $(1)/etc/init.d/prometheus-push
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) ./files/usr/bin/prometheus-push $(1)/usr/bin/prometheus-push
	$(INSTALL_BIN) ./files/usr/bin/pingbot $(1)/usr/bin/pingbot
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/prometheus-push $(1)/etc/config/prometheus-push
	$(INSTALL_DIR) $(1)/usr/share/ucode/node-exporter/lib
	$(INSTALL_DATA) ./files/usr/share/ucode/node-exporter/lib/*.uc $(1)/usr/share/ucode/node-exporter/lib/
endef

$(eval $(call BuildPackage,falter-berlin-statistics))
