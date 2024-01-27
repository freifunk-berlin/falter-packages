
include $(TOPDIR)/rules.mk

PKG_NAME:=falter-common
PKG_RELEASE:=5

PKG_BUILD_DIR := $(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef


define Package/falter-common/template
	SECTION:=luci
	CATEGORY:=LuCI
	SUBMENU:=9. Freifunk
	URL:=https://github.com/freifunk-berlin/falter-packages
	PKGARCH:=all
endef


define Package/falter-common
	$(call Package/falter-standard/template)
	TITLE:=Falter common files
	EXTRA_DEPENDS:=uci, libuci-lua, lua, ip, ethtool, iwinfo, libiwinfo-lua,
	EXTRA_DEPENDS+= uhttpd, uhttpd-mod-ubus, luci, luci-ssl, luci-app-opkg, luci-i18n-base-de, luci-i18n-opkg-de, luci-proto-ppp, luci-theme-bootstrap,
	EXTRA_DEPENDS+= luci-mod-falter, luci-i18n-falter-de, luci-app-falter-owm, luci-app-falter-owm-ant, luci-app-falter-owm-cmd, luci-app-falter-owm-gui,
	EXTRA_DEPENDS+= olsrd, olsrd-mod-arprefresh, olsrd-mod-dyn-gw, olsrd-mod-jsoninfo, olsrd-mod-txtinfo, olsrd-mod-nameservice, olsrd-mod-watchdog, kmod-ipip, luci-app-olsr, luci-app-olsr-services, luci-i18n-olsr-de,
	EXTRA_DEPENDS+= vnstat, luci-app-statistics, luci-i18n-statistics-de, collectd, collectd-mod-dhcpleases, collectd-mod-interface, collectd-mod-iwinfo, collectd-mod-network, collectd-mod-olsrd, collectd-mod-rrdtool, collectd-mod-ping, collectd-mod-uptime, collectd-mod-memory,
	EXTRA_DEPENDS+= tcpdump-mini, mtr, iperf3, tmux
endef

define Package/falter-common/description
	Common configs and scripts that enable participation in Falter-based Freifunk networks.
endef

define Package/falter-common/conffiles
/etc/config/freifunk
endef

FALTER_REVISION:=$(shell git -C $(TOPDIR)/feeds/falter describe --exact-match --tags 2> /dev/null || git -C $(TOPDIR)/feeds/falter rev-parse --short HEAD)

define Package/falter-common/install
	$(CP) ./files/* $(1)/
	$(SED) 's,%R,$(FALTER_REVISION),g' $(1)/etc/freifunk_release
endef


define Package/falter-more
	$(call Package/falter-standard/template)
	TITLE:=Falter more files
	EXTRA_DEPENDS:=falter-common, falter-profiles, luci-app-ffwizard-falter, falter-berlin-migration, falter-berlin-tunneldigger, falter-policyrouting, falter-berlin-ssid-changer, falter-berlin-bbbdigger,
	EXTRA_DEPENDS+= qos-scripts, firewall4, luci-app-firewall, luci-i18n-firewall-de,
	EXTRA_DEPENDS+= falter-berlin-autoupdate, falter-berlin-autoupdate-keys, luci-app-falter-autoupdate, luci-i18n-falter-autoupdate-de,
	EXTRA_DEPENDS+= falter-berlin-service-registrar, luci-app-falter-service-registrar, luci-i18n-falter-service-registrar-de,
	EXTRA_DEPENDS+= collectd-mod-cpu, collectd-mod-load
endef

define Package/falter-more/description
	Additional configs and scripts that enable participation in Falter-based Freifunk networks.
endef

define Package/falter-more/install
	$(CP) ./files/* $(1)/
endef


define Package/falter-standard
	$(call Package/falter-standard/template)
	TITLE:=Falter Standard firmware (tunnel uplink)
	EXTRA_DEPENDS:=falter-more, falter-berlin-uplink-tunnelberlin, wpad-mini
endef

define Package/falter-standard/description
	Complete firmware for Freifunk networks, including OLSR meshing, Tunneldigger uplinks, Web UI, service hosting, and automatic updates.
endef

define Package/falter-standard/install
	true
endef


define Package/falter-notunnel
	$(call Package/falter-standard/template)
	TITLE:=Falter No-Tunnel firmware (direct uplink)
	EXTRA_DEPENDS:=falter-more, falter-berlin-uplink-notunnel
endef

define Package/falter-notunnel/description
	Complete firmware for Freifunk networks, using direct uplink instead of Tunneldigger.
endef

define Package/falter-notunnel/install
	true
endef


define Package/falter-backbone
	$(call Package/falter-standard/template)
	TITLE:=Falter Backbone firmware (mesh only)
	EXTRA_DEPENDS:=falter-common, falter-berlin-tunneldigger
endef

define Package/falter-backbone/description
	Firmware for Freifunk backbone routers, without uplink nor automatic updates.
endef

define Package/falter-backbone/install
	true
endef


$(eval $(call BuildPackage,falter-common))
$(eval $(call BuildPackage,falter-more))
$(eval $(call BuildPackage,falter-standard))
$(eval $(call BuildPackage,falter-notunnel))
$(eval $(call BuildPackage,falter-backbone))
