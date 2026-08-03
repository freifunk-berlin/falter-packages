# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2026 Freifunk Berlin

include $(TOPDIR)/rules.mk

PKG_NAME:=adblock-lean
PKG_VERSION:=0.8.1
PKG_RELEASE:=1

PKG_SOURCE_TAG:=v$(PKG_VERSION)
PKG_SOURCE_VERSION:=38131f9c153d2cbe0f462008217e379da50c7693
PKG_SOURCE:=v$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://github.com/lynxthecat/adblock-lean/archive/refs/tags
PKG_HASH:=84ec7aceca2cb01fc248d595819ecc60fa7b8eaca9e58acba5cb3b036aba2c9e

PKG_MAINTAINER:=Freifunk Berlin <config@berlin.freifunk.net>
PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=LICENCE.md
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

Build/Compile=

define Package/adblock-lean
	SECTION:=net
	CATEGORY:=Network
	TITLE:=Lean dnsmasq-based ad blocker
	URL:=https://github.com/lynxthecat/adblock-lean
	PKGARCH:=all
	EXTRA_DEPENDS:=ca-bundle (>=0), uclient-fetch (>=0), jsonfilter (>=0), gawk (>=0), sed (>=0), coreutils-sort (>=0)
	# adblock-fast manages the same dnsmasq addnmount/blocklist state; bbb-configs
	# never selects both at once, but declare the conflict at the package level
	# too so a broken image build fails loudly instead of installing both.
	# NOTE (verified by building this package): this is honored for the
	# ipk/opkg packaging path (package-pack.mk emits a Conflicts: control
	# field from this variable), but the apk packaging path in this OpenWrt
	# buildroot version does not currently propagate CONFLICTS into the
	# generated .apk metadata at all (`apk mkpkg` has no conflicts concept
	# and the buildroot doesn't fold it into `depends` as `!pkgname` the way
	# Alpine's apk format would need). For apk-built images the Ansible-side
	# adblock_provider guard in conditional_packages.yml remains the only
	# enforcement. Revisit once OpenWrt's apk backend supports this.
	CONFLICTS:=adblock-fast
endef

define Package/adblock-lean/description
adblock-lean is a dnsmasq-based ad blocker for OpenWrt. This package
installs only the upstream runtime files. Site policy, blocklist selection,
cron scheduling, dnsmasq addnmount integration and service enablement are
intentionally managed outside the package by bbb-configs.
endef

define Package/adblock-lean/install
	$(INSTALL_DIR) $(1)/etc/adblock-lean
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_DIR) $(1)/usr/lib/adblock-lean
	$(INSTALL_DIR) $(1)/usr/share/doc/adblock-lean

	$(INSTALL_BIN) $(PKG_BUILD_DIR)/adblock-lean $(1)/etc/init.d/adblock-lean
	$(SED) 's/^ABL_VERSION=.*/ABL_VERSION=$(PKG_VERSION)/' $(1)/etc/init.d/adblock-lean
	grep -q '^ABL_VERSION=$(PKG_VERSION)$$$$' $(1)/etc/init.d/adblock-lean || \
		{ echo "ERROR: failed to patch ABL_VERSION in adblock-lean init script - upstream format may have changed, update the sed pattern in this Makefile"; exit 1; }
	$(SED) 's/^[[:space:]]*check_for_updates.*/	: # check_for_updates disabled in managed package/' $(1)/etc/init.d/adblock-lean
	[ "$$$$(grep -c '# check_for_updates disabled in managed package' $(1)/etc/init.d/adblock-lean)" = 2 ] || \
		{ echo "ERROR: expected exactly 2 check_for_updates call sites patched in adblock-lean init script, got a different count - upstream format may have changed, update the sed pattern in this Makefile (self-update must stay disabled)"; exit 1; }
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/usr/lib/adblock-lean/abl-lib.sh $(1)/usr/lib/adblock-lean/abl-lib.sh
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/usr/lib/adblock-lean/abl-process.sh $(1)/usr/lib/adblock-lean/abl-process.sh
	$(SED) 's/--timeout=3/--timeout=15/' $(1)/usr/lib/adblock-lean/abl-process.sh
	grep -q -- '--timeout=15' $(1)/usr/lib/adblock-lean/abl-process.sh || \
		{ echo "ERROR: failed to patch download timeout in abl-process.sh - upstream format may have changed, update the sed pattern in this Makefile"; exit 1; }
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/LICENCE.md $(1)/usr/share/doc/adblock-lean/LICENCE.md
	md5sum \
		$(1)/etc/init.d/adblock-lean \
		$(1)/usr/lib/adblock-lean/abl-lib.sh \
		$(1)/usr/lib/adblock-lean/abl-process.sh \
		| sed 's#$(1)##' > $(1)/etc/adblock-lean/abl-reg.md5
endef

$(eval $(call BuildPackage,adblock-lean))
