include $(TOPDIR)/rules.mk

PKG_NAME:=falter-monitor-pushgw
PKG_RELEASE:=0

PKG_BUILD_DIR := $(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/falter-monitor-pushgw
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=Freifunk Monitor Statistics Collector
  EXTRA_DEPENDS:=prometheus-node-exporter-ucode (>=0), prometheus-node-exporter-ucode-openwrt (>=0), prometheus-node-exporter-ucode-wifi (>=0)
  PKGARCH:=all
endef

define Package/falter-berlin-gw/description
  Tool send, line go up!
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/falter-monitor-pushgw/install
	$(CP) ./files/freifunk.uc $(1)/usr/share/ucode/node-exporter/lib/
endef

$(eval $(call BuildPackage,falter-berlin-gw))
