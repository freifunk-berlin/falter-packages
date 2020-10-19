#!/bin/sh
#
# As part of the install, we don't want to smash an already set up config
# but at the same time update it if necessary.  The unique attributes are:
#
# network.bbbdigger_dev.macaddr
# tunneldigger.bbbdigger.uuid
#
# All other config sections are overwritten with current settings

. /lib/functions.sh

TUNNEL_SERV='a.bbb-vpn.berlin.freifunk.net:8942 b.bbb-vpn.berlin.freifunk.net:8942'
IFACE=bbbdigger
BIND=wan

# tunneldigger UUID (and MAC) generation, if there isn't one already
# See the website https://www.itwissen.info/MAC-Adresse-MAC-address.html
MAC=$(uci -q get network.${IFACE}_dev.macaddr)
if [ $? -eq 1 ]; then
  # start with b6 for Berliner 6ackbone
  MAC="b6"
  for byte in 2 3 4 5 6; do
    MAC=$MAC`dd if=/dev/urandom bs=1 count=1 2> /dev/null | hexdump -e '1/1 ":%02x"'`
  done
fi

UUID=$(uci -q get tunneldigger.${IFACE}.uuid)
if [ $? -eq 1 ]; then
  UUID=$MAC
  for byte in 7 8 9 10; do
    UUID=$UUID`dd if=/dev/urandom bs=1 count=1 2> /dev/null | hexdump -e '1/1 ":%02x"'`
  done
fi

# tunneldigger setup
uci set tunneldigger.$IFACE=broker
uci -q delete tunneldigger.$IFACE.address
for srv in $TUNNEL_SERV; do
  uci add_list tunneldigger.$IFACE.address=$srv
done
uci set tunneldigger.$IFACE.uuid=$UUID
uci set tunneldigger.$IFACE.interface=$IFACE
uci set tunneldigger.$IFACE.broker_selection=usage
uci set tunneldigger.$IFACE.bind_interface=$BIND
uci set tunneldigger.$IFACE.enabled=1

# network setup
uci set network.${IFACE}_dev=device
uci set network.${IFACE}_dev.macaddr=$MAC
uci set network.${IFACE}_dev.name=$IFACE

uci set network.$IFACE=interface
uci set network.$IFACE.proto=dhcp
uci set network.$IFACE.ifname=$IFACE

# firewall setup (first remove from the zone and add it back)
ZONE=$(echo $(uci show firewall.zone_freifunk.network | cut -d \' -f 2 | sed "s/${IFACE}//g"))
ZONE="$ZONE $IFACE"
uci set firewall.zone_freifunk.network="${ZONE}"

# olsr setup (first remove it and add it again)
SECTION=$(uci show olsrd | grep ${IFACE} | cut -d . -f 1-2)
[ ! -z $SECTION ] && uci delete $SECTION
uci add olsrd Interface
uci set olsrd.@Interface[-1].ignore=0
uci set olsrd.@Interface[-1].interface=$IFACE
uci set olsrd.@Interface[-1].Mode=ether

#uci changes

uci commit
reload_config
/etc/init.d/tunneldigger restart
