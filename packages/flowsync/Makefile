# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2026 Simon Polack <spolack+git@mailbox.org>
#

include $(TOPDIR)/rules.mk

PKG_NAME:=flowsync
PKG_VERSION:=2.0
PKG_RELEASE:=1

PKG_MAINTAINER:=Simon Polack <spolack+git@mailbox.org>
PKG_LICENSE:=GPL-2.0-only

# the tc programs include kernel UAPI headers for the BPF target
PKG_BUILD_DEPENDS:=bpf-headers

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/bpf.mk

# libbpf loads the tc programs and talks to their maps; kmod-sched-core and
# kmod-sched-bpf are the clsact qdisc and the BPF classifier they attach
# with. The rules are read and written with nft, which firewall4 brings
# (with kmod-nft-fib, for the notrack rule).
define Package/flowsync
	SECTION:=net
	CATEGORY:=Network
	TITLE:=Flow tables for active-active gateways without conntrack
	URL:=https://github.com/freifunk-berlin/falter-packages
	DEPENDS:=+libbpf +kmod-sched-core +kmod-sched-bpf +firewall4 $(BPF_DEPENDS)
	# the daemon drops to this user (CAP_NET_ADMIN only) once the programs
	# are loaded and its sockets open, see --user in the README
	USERID:=flowsync=3780:flowsync=3780
endef

define Package/flowsync/description
Forwarded IPv6 bypasses conntrack: two tc programs on the uplink keep the
flows a gateway forwarded out in a BPF map and mark the replies; the daemon
announces those flows to the other gateways, which accept the replies
arriving on an asymmetric path the same way. Replaces conntrackd +
samplicator.
endef

define Package/flowsync/conffiles
/etc/config/flowsync
endef

# stopping the service leaves the programs on the uplink, the maps and the
# rules (replies keep passing across a restart). With the package gone nobody
# would look after them: take them away first, forwarded IPv6 is conntrack's
# again. (apk runs this behind the default prerm, which stops and disables
# the service; detach stops it itself.)
define Package/flowsync/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || /etc/init.d/flowsync detach
exit 0
endef

# only the sources: src/ may also hold a host build from development (objects,
# the binary, local.mk) that the default cp -fpR would carry along, newer than
# the sources, so that make would not rebuild them for the target
define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/bpf
	$(CP) ./src/*.c ./src/*.h ./src/Makefile $(PKG_BUILD_DIR)/
	$(CP) ./src/bpf/flowsync.bpf.c $(PKG_BUILD_DIR)/bpf/
endef

define Build/Configure
endef

# the daemon with the target compiler (the default Build/Compile), the tc
# programs with the BPF toolchain
MAKE_FLAGS += LDLIBS="-lbpf"

define Build/Compile
	$(call CompileBPF,$(PKG_BUILD_DIR)/bpf/flowsync.bpf.c)
	$(call Build/Compile/Default,flowsync)
endef

define Package/flowsync/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/flowsync $(1)/usr/sbin/flowsync

	$(INSTALL_DIR) $(1)/lib/bpf
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/bpf/flowsync.bpf.o $(1)/lib/bpf/flowsync.o

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/flowsync.init $(1)/etc/init.d/flowsync

	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/flowsync.config $(1)/etc/config/flowsync

	# fw4 includes it in its forward chain on every (re)load: behind the
	# includes its own configuration prepends, before its ct state rule
	$(INSTALL_DIR) $(1)/usr/share/nftables.d/chain-pre/forward
	$(INSTALL_DATA) ./files/flowsync.nft $(1)/usr/share/nftables.d/chain-pre/forward/10-flowsync.nft
endef

$(eval $(call BuildPackage,flowsync))
