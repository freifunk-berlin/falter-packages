# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2026 Simon Polack <spolack+git@mailbox.org>
#

include $(TOPDIR)/rules.mk

PKG_NAME:=flowsync
PKG_VERSION:=1.0
PKG_RELEASE:=1

PKG_MAINTAINER:=Simon Polack <spolack+git@mailbox.org>
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

# Build and runtime: the binary links libnetfilter_conntrack (BPF filter for
# the event socket) and libmnl; their headers are all the build needs, so
# there is no PKG_BUILD_DEPENDS. libnfnetlink comes in through
# libnetfilter-conntrack and is not linked directly.
#
# Runtime only: ctnetlink in the kernel. Not a build dependency, so it goes
# into EXTRA_DEPENDS (which does not order the build) with the kernel floor
# the loop prevention relies on (UDP tracker semantics, see README).
define Package/flowsync
	SECTION:=net
	CATEGORY:=Network
	TITLE:=Scoped conntrack flow announcer for active-active gateways
	URL:=https://github.com/freifunk-berlin/falter-packages
	DEPENDS:=+libnetfilter-conntrack +libmnl
	EXTRA_DEPENDS:=kmod-nf-conntrack-netlink (>=6.12)
endef

define Package/flowsync/description
Announces new and existing IPv6 UDP and TCP conntrack entries of selected client
prefixes to the other gateways, which inject them into their conntrack
table with a short timeout, so that replies arriving on an asymmetric path
pass the stateful firewall. Replaces conntrackd + samplicator.
endef

define Package/flowsync/conffiles
/etc/config/flowsync
endef

define Build/Configure
endef

# default Build/Compile: make -C $(PKG_BUILD_DIR) with the target CC/CFLAGS/LDFLAGS
MAKE_FLAGS += LDLIBS="-lnetfilter_conntrack -lmnl"

define Package/flowsync/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/flowsync $(1)/usr/sbin/flowsync

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/flowsync.init $(1)/etc/init.d/flowsync

	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/flowsync.config $(1)/etc/config/flowsync
endef

$(eval $(call BuildPackage,flowsync))
