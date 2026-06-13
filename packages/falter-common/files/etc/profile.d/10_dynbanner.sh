#!/bin/sh

# This script originates from Freifunk Berlin. It gets dynamic information
# and prints them for user information below the banner.
# It is licensed under GNU General Public License v3.0 or later
# Copyright (C) 2021   Martin Hübner

# shellcheck shell=dash

COMMUNITY=$(uci -q get freifunk.community.name)
SUFFIX=$(uci -q get profile_${COMMUNITY}.profile.suffix)
if [ 1 -eq $? ]; then
    SUFFIX="ff"
fi

printf "\
 Falter %s (%s) %s
 https://wiki.freifunk.net/Berlin:Firmware
 https://github.com/freifunk-berlin/falter-packages
 -----------------------------------------------------\n
 If you find bugs please report them at:\n
   https://github.com/freifunk-berlin/falter-packages/issues/\n
 For questions write a mail to <berlin@berlin.freifunk.net>
 or check https://berlin.freifunk.net/contact for our weekly meetings.

 Host.............................: %s
 Hardware.........................: %s
 IP-Address.......................: %s
 Uptime...........................: %s
 Free flash.......................: %s
 Average load (1m, 5m, 15m).......: %s
 DHCP-Clients.....................: %s

" \
 "$(sed -n "s/'//g;s/^FREIFUNK_RELEASE=//p" /etc/freifunk_release)"\
 "$(sed -n "s/'//g;s/^FREIFUNK_REVISION=//p" /etc/freifunk_release)"\
 "$(sed -n "s/'//g;s/^DISTRIB_TARGET=//p" /etc/openwrt_release)"\
 "$(uci -q get system.@system[0].hostname).${SUFFIX}"\
 "$(cat /tmp/sysinfo/model)"\
 "$(uci -q get network.dhcp.ipaddr)"\
 "$(uptime | sed 's/^.*up *//;s/,.*$//')"\
 "$(df -h | grep ' /overlay' | tr -s ' ' | cut -d ' ' -f 4)"\
 "$(uptime | sed 's/.*: //')"\
 "$(wc -l /tmp/dhcp.leases | cut -d ' ' -f 1)"
