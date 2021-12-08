include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-hna-list
PKG_RELEASE:=3

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/falter-berlin-hna-list
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=List HNAs in OLSR network
  PROVIDES:=hna-list
endef

TARGET_CFLAGS += \
	-I$(STAGING_DIR)/usr/include/libnl-tiny \
	-I$(STAGING_DIR)/usr/include \
	-DLIBNL_TINY

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) $(PKG_BUILD_DIR)/src/* $(PKG_BUILD_DIR)
endef

define Package/falter-berlin-hna-list/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/hna-list $(1)/usr/bin/hna-list
endef

$(eval $(call BuildPackage,falter-berlin-hna-list))
