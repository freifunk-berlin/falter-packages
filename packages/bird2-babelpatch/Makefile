#
# Copyright (C) 2009-2017 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.

include $(TOPDIR)/rules.mk

PKG_NAME:=bird2-babelpatch
PKG_VERSION:=2.17.1
PKG_RELEASE:=1

PKG_SOURCE:=bird-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://bird.network.cz/download/
PKG_HASH:=bfd718dfa596819b3801688783212514b467163329aec9bbcd0fa3dee03e10e9

PKG_MAINTAINER:=Toke Høiland-Jørgensen <toke@toke.dk>
PKG_LICENSE:=GPL-2.0-or-later

PKG_BUILD_DEPENDS:=ncurses readline
PKG_BUILD_DIR:=$(BUILD_DIR)/bird-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/bird2-babelpatch/Default/description
BIRD is an internet routing daemon which manages TCP/IP routing tables
with support of modern routing protocols, easy to use configuration
interface and powerful route filtering language. It is lightweight and
efficient and therefore appropriate for small embedded routers.

endef

define Package/bird2-babelpatch
  TITLE:=The BIRD Internet Routing Daemon (v2)
  URL:=http://bird.network.cz/
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Routing and Redirection
  PROVIDES:=bird2
  DEPENDS:=+libpthread
  CONFLICTS:=bird1-ipv4 bird1-ipv6 bird4 bird6 bird2
endef

define Package/bird2-babelpatch/description
$(call Package/bird2-babelpatch/Default/description)

BIRD supports OSPFv2, RIPv2, Babel and BGP protocols for IPv4 and
OSPFv3, RIPng, Babel and BGP protocols for IPv6.

In BGP, BIRD supports communities, multiprotocol extensions, MD5
authentication, 32bit AS numbers and could act as a route server or a
route reflector. BIRD also supports multiple RIBs, multiple kernel
routing tables and redistribution between the protocols with a powerful
configuration syntax.

This is the 2.0 branch of Bird which integrates support for IPv4 and IPv6
into a single branch, and also adds support for the Babel routing protocol.
endef

CONFIGURE_ARGS += --disable-libssh

define Package/bird2/conffiles
/etc/bird.conf
/etc/bird4.conf
/etc/bird6.conf
endef

define Package/bird2-babelpatch/install
	$(INSTALL_DIR)  $(1)/usr/sbin
	$(INSTALL_BIN)  $(PKG_BUILD_DIR)/bird $(1)/usr/sbin/
	$(INSTALL_DIR)  $(1)/etc
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/doc/bird.conf.example $(1)/etc/bird.conf
	$(INSTALL_DIR)  $(1)/etc/init.d
	$(INSTALL_BIN)  ./files/bird.init $(1)/etc/init.d/bird
endef

$(eval $(call BuildPackage,bird2-babelpatch))
