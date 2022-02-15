#!/bin/sh

interface="$1"

uci revert olsrd
section="$(uci show olsrd | grep $interface | awk -F '.interface' '{print $1}')"
uci delete "$section"
uci commit olsrd

# delete interface via ipc instead of reloading to make it seamless
ubus call olsrd del_interface '{"ifname":'\""$interface"\"'}'

uci revert babeld
section="$(uci show babeld | grep $interface | awk -F '.ifname' '{print $1}')"
uci delete "$section"
uci commit babeld
/etc/init.d/babeld reload
