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

# libbpf loads the tc programs and talks to their maps. They attach to the
# uplink's tcx hooks (CONFIG_NET_XGRESS, which BPF_SYSCALL
# selects): no qdisc and no scheduler module is involved, SQM and the like
# keep theirs.
#
# The rules are read and written with the nft tool, and the notrack rule
# needs the fib expression. Both are used on the device only and nothing
# here links against them: they are EXTRA_DEPENDS, which go into the
# package's dependency list and nowhere else (DEPENDS would also have the
# build system build nftables first). EXTRA_DEPENDS must name a version:
# any nftables 1.x has what the rules use, and every variant provides the
# name; the module's version is the kernel's, whose minimum is 6.12 (see
# README.md). No firewall is required: the accept rule goes into the forward
# chain of whatever inet table fw_table names (fw4 by default).
define Package/flowsync
	SECTION:=net
	CATEGORY:=Network
	TITLE:=Flow tables for active-active gateways without conntrack
	URL:=https://github.com/freifunk-berlin/falter-packages
	DEPENDS:=+libbpf $(BPF_DEPENDS)
	EXTRA_DEPENDS:=nftables (>=1.0.0), kmod-nft-fib (>=6.12)
	# the daemon drops to this user (CAP_NET_ADMIN only) once the programs
	# are loaded and its sockets open, see --user in the README
	USERID:=flowsync:flowsync
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
# again. Not on an upgrade: opkg runs the old package's prerm then (apk runs
# pre-upgrade/post-upgrade instead), and the datapath stays for the new
# daemon. (apk runs this behind the default prerm, which stops and disables
# the service; detach stops it itself.)
define Package/flowsync/prerm
#!/bin/sh
[ "$${PKG_UPGRADE}" = 1 ] || [ -n "$${IPKG_INSTROOT}" ] || /etc/init.d/flowsync detach
exit 0
endef

# after an upgrade the default postinst's "start" changes nothing on a
# running service: restart it, so that the new daemon loads the new object
# and replaces the programs in place (the maps and the alive element stay)
define Package/flowsync/postinst
#!/bin/sh
[ "$${PKG_UPGRADE}" = 1 ] && [ -z "$${IPKG_INSTROOT}" ] && /etc/init.d/flowsync restart
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

	# for firewall4, which includes it in its forward chain on every
	# (re)load: behind the includes its own configuration prepends, before
	# its ct state rule. Nothing else reads it; the daemon inserts the rule
	# itself where it is missing
	$(INSTALL_DIR) $(1)/usr/share/nftables.d/chain-pre/forward
	$(INSTALL_DATA) ./files/flowsync.nft $(1)/usr/share/nftables.d/chain-pre/forward/10-flowsync.nft
endef

$(eval $(call BuildPackage,flowsync))
