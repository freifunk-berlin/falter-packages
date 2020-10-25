#!/bin/sh

# Get dynamic information and print them under the banner.

HOSTNAME=$(uci -q get system.@system[0].hostname)".olsr"
IPADDR=$(uci -q get network.dhcp.ipaddr)
UPTIME=$(uptime | cut -d ',' -f 0 | cut -d ' ' -f 4-)
FREEFL=$(df -h | grep " /overlay" | sed -E -e s/[[:space:]]+/\;/g | cut -d';' -f4 )
SYS_LOAD=$(uptime | sed -e 's/average: /;/g' | cut -d';' -f2)
CLIENTS=$(wc -l /tmp/dhcp.leases | cut -d' ' -f1)

printf \
" Host.............................: $HOSTNAME
 IP-Address.......................: $IPADDR
 Uptime...........................: $UPTIME
 Free flash.......................: $FREEFL
 Average load (1m, 5m, 15m).......: $SYS_LOAD
 DHCP-Clients.....................: $CLIENTS


"
