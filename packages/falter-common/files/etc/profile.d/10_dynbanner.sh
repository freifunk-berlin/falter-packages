#!/bin/sh

# This script originates from Freifunk Berlin. It gets dynamic information
# and prints them for user information below the banner.
# It is licensed under GNU General Public License v3.0 or later
# Copyright (C) 2021   Martin Hübner

# shellcheck shell=dash

HOSTNAME=$(uci -q get system.@system[0].hostname)".olsr"
IPADDR=$(uci -q get network.dhcp.ipaddr)
UPTIME=$(uptime | cut -d ',' -f 0 | cut -d ' ' -f 4-) > /dev/null 2>&1
FREEFL=$(df -h | grep " /overlay" | sed -E -e s/[[:space:]]+/\;/g | cut -d';' -f4 ) > /dev/null 2>&1
SYS_LOAD=$(cut -d' ' -f 1-3 < /proc/loadavg ) > /dev/null 2>&1
CLIENTS=$(wc -l /tmp/dhcp.leases | cut -d' ' -f1) > /dev/null 2>&1

printf \
" Host.............................: %s
 IP-Address.......................: %s
 Uptime...........................: %s
 Free flash.......................: %s
 Average load (1m, 5m, 15m).......: %s
 DHCP-Clients.....................: %s


" "$HOSTNAME" "$IPADDR" "$UPTIME" "$FREEFL" "$SYS_LOAD" "$CLIENTS"
