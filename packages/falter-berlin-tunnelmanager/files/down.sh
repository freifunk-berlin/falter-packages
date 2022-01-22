#!/bin/sh

interface="$1"



uci revert olsrd
section="$(uci show olsrd | grep $interface | awk -F '.interface' '{print $1}')"
uci delete "$section"
uci commit
/etc/init.d/olsrd reload

uci revert babeld
section="$(uci show babeld | grep $interface | awk -F '.ifname' '{print $1}')"
uci delete "$section"
uci commit
/etc/init.d/babeld reload
