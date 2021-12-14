include $(TOPDIR)/rules.mk

PKG_NAME:=falter-berlin-hna-list
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

SOURCE_DIR:=/home/martin/Freifunk/dev/falter-packages/packages/falter-berlin-hna-list/src/

define Package/falter-berlin-hna-list
  SECTION:=falter-berlin
  CATEGORY:=falter-berlin
  TITLE:=List HNAs in OLSR network
  PROVIDES:=hna-list
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) -r $(SOURCE_DIR) $(PKG_BUILD_DIR)
	$(Build/Patch)
endef

# Package build instructions; invoke the target-specific compiler to first compile the source file, and then to link the file into the final executable
define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -o $(PKG_BUILD_DIR)/main.o -c $(PKG_BUILD_DIR)/src/main.c 
	$(TARGET_CC) $(TARGET_CFLAGS) -o $(PKG_BUILD_DIR)/avl.o -c $(PKG_BUILD_DIR)/src/libavl/avl.c
	$(TARGET_CC) $(TARGET_LDFLAGS) -o $(PKG_BUILD_DIR)/hna-list $(PKG_BUILD_DIR)/main.o $(PKG_BUILD_DIR)/avl.o
endef

define Package/falter-berlin-hna-list/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/hna-list $(1)/usr/bin/hna-list
endef

$(eval $(call BuildPackage,falter-berlin-hna-list))
