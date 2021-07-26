#!/bin/sh

# Get dynamic information and print them under the banner.

HOSTNAME=$(uci -q get system.@system[0].hostname)".olsr"
IPADDR=$(uci -q get network.dhcp.ipaddr)
UPTIME=$(uptime | cut -d ',' -f 0 | cut -d ' ' -f 4-) 2&> /dev/null
FREEFL=$(df -h | grep " /overlay" | sed -E -e s/[[:space:]]+/\;/g | cut -d';' -f4 ) 2&> /dev/null
SYS_LOAD=$(cat /proc/loadavg | cut -d' ' -f 1-3) 2&> /dev/null
CLIENTS=$(wc -l /tmp/dhcp.leases | cut -d' ' -f1) 2&> /dev/null

printf \
" Host.............................: $HOSTNAME
 IP-Address.......................: $IPADDR
 Uptime...........................: $UPTIME
 Free flash.......................: $FREEFL
 Average load (1m, 5m, 15m).......: $SYS_LOAD
 DHCP-Clients.....................: $CLIENTS


"
