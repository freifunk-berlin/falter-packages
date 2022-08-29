#!/bin/sh

#shellcheck shell=dash

# minor stuff. Not important in this place
# shellcheck disable=SC2034

# shellcheck source=/dev/null
. /lib/functions/guard.sh

create_ffuplink() {
  uci -q delete network.ffuplink
  # create a very basic ffuplink interface
  uci set network.ffuplink=interface
  uci set network.ffuplink.device=ffuplink
  uci set network.ffuplink.peerdns=0
  # see https://github.com/freifunk-berlin/firmware/issues/561
  uci set network.ffuplink.ip4table=ffuplink
  uci set network.ffuplink.ip6table=ffuplink
  # the following options need to be set by the individual uplink-package
  uci set network.ffuplink.proto=none
  uci commit network.ffuplink

  guard_delete notunnel
  guard_delete tunnelberlin_openvpn
  guard_delete tunnelberlin_tunneldigger
  guard_delete vpn03_openvpn
}

generate_random_mac_hex() {
  local prefix=$1

  [ -n "$prefix" ] || return
  local macaddr
  # Create a static macaddr starting with "prefix" for ffuplink devices
  # See the website https://www.itwissen.info/MAC-Adresse-MAC-address.html
  for byte in 2 3 4 5 6; do
    macaddr=$macaddr$(dd if=/dev/urandom bs=1 count=1 2> /dev/null | hexdump -e '1/1 ":%02x"')
  done

  echo "$prefix$macaddr"
}
