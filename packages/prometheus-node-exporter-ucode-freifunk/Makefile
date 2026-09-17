# Copyright (C) 2013-2017 OpenWrt.org

include $(TOPDIR)/rules.mk

PKG_NAME:=prometheus-node-exporter-ucode-freifunk
PKG_VERSION:=2024.02.07
PKG_RELEASE:=3

PKG_BUILD_DIR := $(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef


define Package/$(PKG_NAME)/template
	SECTION:=falter-berlin
	CATEGORY:=falter-berlin
	SUBMENU:=9. Freifunk
	URL:=https://github.com/freifunk-berlin/falter-packages
	PKGARCH:=all
endef

define Package/$(PKG_NAME)
	$(call Package/$(PKG_NAME)/template)
	TITLE:==Prometheus node exporter extensions for Freifunk Falter
	EXTRA-DEPENDS:=prometheus-node-exporter-ucode (>=0)
	EXTRA-DEPENDS+=prometheus-node-exporter-ucode-openwrt (>=0)
	EXTRA-DEPENDS+=ucode-mod-resolv (<=0)
endef

define Package/$(PKG_NAME)/description
	Prometheus node exporter extensions for Freifunk Falter and a push service
endef

define Package/$(PKG_NAME)/install
	$(CP) ./files/* $(1)/
endef

$(eval $(call BuildPackage,prometheus-node-exporter-ucode-freifunk))
