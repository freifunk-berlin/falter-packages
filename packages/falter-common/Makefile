
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
	EXTRA_DEPENDS:=uci (>=0), libuci-lua (>=0), lua (>=0), ip-tiny (>=0), ethtool (>=0), iwinfo (>=0), libiwinfo-lua (>=0),
	EXTRA_DEPENDS+= uhttpd (>=0), uhttpd-mod-ubus (>=0), luci (>=0), luci-app-package-manager (>=0), luci-i18n-base-de (>=0), luci-i18n-package-manager-de (>=0), luci-proto-ppp (>=0), luci-theme-bootstrap (>=0),
	EXTRA_DEPENDS+= luci-mod-falter (>=0), luci-i18n-falter-de (>=0), luci-app-falter-owm (>=0), luci-app-falter-owm-ant (>=0), luci-app-falter-owm-cmd (>=0), luci-app-falter-owm-gui (>=0),
	EXTRA_DEPENDS+= olsrd (>=0), olsrd-utils (>=0), olsrd-mod-arprefresh (>=0), olsrd-mod-dyn-gw (>=0), olsrd-mod-jsoninfo (>=0), olsrd-mod-txtinfo (>=0), olsrd-mod-nameservice (>=0), olsrd-mod-watchdog (>=0), kmod-ipip (>=0), luci-app-olsr (>=0), luci-app-olsr-services (>=0), luci-i18n-olsr-de (>=0),
	EXTRA_DEPENDS+= vnstat (>=0), luci-app-statistics (>=0), luci-i18n-statistics-de (>=0), collectd (>=0), collectd-mod-dhcpleases (>=0), collectd-mod-interface (>=0), collectd-mod-iwinfo (>=0), collectd-mod-network (>=0), collectd-mod-olsrd (>=0), collectd-mod-rrdtool (>=0), collectd-mod-ping (>=0), collectd-mod-uptime (>=0), collectd-mod-memory (>=0),
	EXTRA_DEPENDS+= tcpdump-mini (>=0), mtr (>=0), iperf3 (>=0), tmux (>=0)
endef

define Package/falter-common/description
	Common configs and scripts that are shared by Falter and BBB-Configs.
endef

define Package/falter-common/conffiles
/etc/config/freifunk
/etc/config/ffwizard
endef

FALTER_REVISION:=$(shell git -C $(TOPDIR)/feeds/falter describe --exact-match --tags 2> /dev/null || git -C $(TOPDIR)/feeds/falter rev-parse --short HEAD)

define Package/falter-common/install
	$(CP) ./files/* $(1)/
	$(SED) 's,%R,$(FALTER_REVISION),g' $(1)/etc/freifunk_release
endef


define Package/falter-more
	$(call Package/falter-standard/template)
	TITLE:=Falter more packages
	EXTRA_DEPENDS:=falter-common (>=0), falter-profiles (>=0), luci-app-ffwizard-falter (>=0), falter-berlin-migration (>=0), falter-berlin-tunneldigger (>=0), falter-policyrouting (>=0), falter-berlin-ssid-changer (>=0), falter-berlin-bbbdigger (>=0),
	EXTRA_DEPENDS+= qos-scripts (>=0), firewall4 (>=0), iptables-nft (>=0), ip6tables-nft (>=0), luci-app-firewall (>=0), luci-i18n-firewall-de (>=0),
	EXTRA_DEPENDS+= falter-berlin-autoupdate (>=0), falter-berlin-autoupdate-keys (>=0), luci-app-falter-autoupdate (>=0), luci-i18n-falter-autoupdate-de (>=0),
	EXTRA_DEPENDS+= falter-berlin-service-registrar (>=0), luci-app-falter-service-registrar (>=0), luci-i18n-falter-service-registrar-de (>=0),
	EXTRA_DEPENDS+= collectd-mod-cpu (>=0), collectd-mod-load (>=0)
endef

define Package/falter-more/description
	Additional packages that enable participation in Falter-based Freifunk networks.
endef

define Package/falter-more/install
	true
endef


define Package/falter-standard
	$(call Package/falter-standard/template)
	TITLE:=Falter Standard firmware (tunnel uplink)
	EXTRA_DEPENDS:=falter-defaults (>=0), falter-more (>=0), falter-berlin-uplink-tunnelberlin (>=0), luci-ssl (>=0)
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
	EXTRA_DEPENDS:=falter-defaults (>=0), falter-more (>=0), falter-berlin-uplink-notunnel (>=0), luci-ssl (>=0)
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
	EXTRA_DEPENDS:=falter-defaults (>=0), falter-common (>=0), falter-berlin-tunneldigger (>=0), luci-ssl (>=0)
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
