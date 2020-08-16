include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-tunneldigger
PKG_SOURCE_DATE:=2020-08-10
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/wlanslovenija/tunneldigger.git
PKG_SOURCE_VERSION:=b8fd97efbacfc0ec5d67b153bc83eb5dbc1cb1f9
PKG_MIRROR_HASH:=46ab3898a94e92e0d157ecb7e07b523454c3802dd4cf6c64f7f80c5a99922297

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/falter-berlin-tunneldigger
  SECTION:=net
  CATEGORY:=Network
  DEPENDS:=+libnl-tiny
  EXTRA_DEPENDS:=kmod-l2tp, kmod-l2tp-ip, kmod-l2tp-eth, librt, libpthread
  TITLE:=L2TPv3 tunnel broker client
  PROVIDES:=tunneldigger
endef

TARGET_CFLAGS += \
	-I$(STAGING_DIR)/usr/include/libnl-tiny \
	-I$(STAGING_DIR)/usr/include \
	-DLIBNL_TINY

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) $(PKG_BUILD_DIR)/client/* $(PKG_BUILD_DIR)
endef

define Package/falter-berlin-tunneldigger/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/tunneldigger $(1)/usr/bin/tunneldigger
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/tunneldigger.init $(1)/etc/init.d/tunneldigger
	$(INSTALL_DIR) $(1)/etc/hotplug.d/iface
	$(INSTALL_BIN) ./files/tunneldigger.hotplug $(1)/etc/hotplug.d/iface/60-tunneldigger
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_DATA) ./files/config.default $(1)/etc/config/tunneldigger
endef

define Package/falter-berlin-tunneldigger/conffiles
/etc/config/tunneldigger
endef

$(eval $(call BuildPackage,falter-berlin-tunneldigger))
