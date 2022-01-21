#!/bin/sh

interface="$1"


section="$(uci show olsrd | grep \"$interface\" | awk -F '.interface' '{print $1}')"
uci delete "$section"
/etc/init.d/olsrd reload

section="$(uci show babeld | grep \"$interface\" | awk -F '.ifname' '{print $1}')"
uci delete "$section"
/etc/init.d/babeld reload
