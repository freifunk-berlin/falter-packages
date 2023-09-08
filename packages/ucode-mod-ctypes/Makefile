include $(TOPDIR)/rules.mk

PKG_NAME:=ucode-mod-ctypes
PKG_RELEASE:=0
PKG_MAINTAINER:=Packet Please <pktpls@systemli.org>

include $(INCLUDE_DIR)/package.mk

define Package/ucode-mod-ctypes
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=ucode ctypes module
  DEPENDS:=+libucode +libffi
endef

define Package/ucode-mod-ctypes/description
The ctypes module provides functionality for ...
endef

define Package/ucode-mod-ctypes/install
	$(INSTALL_DIR) $(1)/usr/lib/ucode
	$(CP) $(PKG_BUILD_DIR)/ctypes.so $(1)/usr/lib/ucode/
endef

define Build/Configure
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) $(FPIC) \
		-Wall -ffunction-sections -Wl,--gc-sections -shared -Wl,--no-as-needed -lffi \
		-o $(PKG_BUILD_DIR)/ctypes.so $(PKG_BUILD_DIR)/ctypes.c
endef

$(eval $(call BuildPackage,ucode-mod-ctypes))
