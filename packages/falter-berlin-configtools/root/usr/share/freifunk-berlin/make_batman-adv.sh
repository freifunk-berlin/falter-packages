#!/bin/sh

. /lib/functions.sh

create_interface() {
  MASTERIF=$1
  
  uci set network.${MASTERIF}_bat=interface
  uci set network.${MASTERIF}_bat.ifname='@${MASTERIF}'
  uci set network.${MASTERIF}_bat.proto='batadv'
  uci set network.${MASTERIF}_bat.mesh='bat0'
  uci commit network.${MASTERIF}_bat
}

add_batman_iface() {
  DSTBRIDGE=$1
  
  ifnames="$(uci get network.${DSTBRIDGE}.ifname)"
  list_contains ${ifnames} ${BATIF} || uci set network.${DSTBRIDGE}.ifname="${ifnames} ${BATIF}"
}

BATIF=bat0

create_interface wireless0
add_batman_iface dhcp
