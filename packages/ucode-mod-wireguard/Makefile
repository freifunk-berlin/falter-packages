include $(TOPDIR)/rules.mk

PKG_NAME:=ucode-mod-wireguard
PKG_RELEASE:=0
PKG_MAINTAINER:=Packet Please <pktpls@systemli.org>

include $(INCLUDE_DIR)/package.mk

define Package/ucode-mod-wireguard
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=ucode wireguard module
  DEPENDS:=+libucode +libubox +libnl-tiny
endef

define Package/ucode-mod-wireguard/description
The wireguard module provides functionality for creating and configuring
Wireguard interfaces using the kernel generic netlink API.
endef

define Package/ucode-mod-wireguard/install
	$(INSTALL_DIR) $(1)/usr/lib/ucode
	$(CP) $(PKG_BUILD_DIR)/wireguard.so $(1)/usr/lib/ucode/
endef

define Build/Configure
endef

TARGET_CFLAGS += \
	-D_GNU_SOURCE \
	-I$(STAGING_DIR)/usr/include/libnl-tiny

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) $(FPIC) \
		-Wall -ffunction-sections -Wl,--gc-sections -shared -Wl,--no-as-needed -lubox -lnl-tiny \
		-o $(PKG_BUILD_DIR)/wireguard.so $(PKG_BUILD_DIR)/wireguard.c
endef

$(eval $(call BuildPackage,ucode-mod-wireguard))
