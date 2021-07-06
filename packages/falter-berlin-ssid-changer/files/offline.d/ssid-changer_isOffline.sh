#!/bin/sh
. /lib/functions.sh
. /lib/lib_ssid-changer.sh

# We do assume, that both radios (if there is more than one) share the same
# SSID. Handling different SSIDS is too complicated.

# leave the SSID as is, if there is still some connection to the net.
[ "$GLOBAL" = "ONLINE" ] && exit

is_internet_reachable
[ $? = 0 ] && exit

ONLINE_SSIDS=$(get_interfaces)
LOGMSG="Didn't reach the internet. Change SSID to offline..."

# abort if SSID is "offline" already
for HOSTAPD in $(ls /var/run/hostapd-phy*); do
    CURRSSID=$(grep -e "^ssid=" $HOSTAPD | cut -d'=' -f2)
    if [ "$CURRSSID" = "$OFFLINE_SSID" ]; then
        logger -s -t "ssid-changer" -p 5 "SSID offline already. Nothing to change."
        exit 0
    fi
done

# loop over hostapd configs and try to switch any matching ID.
for HOSTAPD in $(ls /var/run/hostapd-phy*); do
    for ONLINE_SSID in $ONLINE_SSIDS; do
        logger -s -t "ssid-changer" -p 5 "$LOGMSG"
        sed -i "s~^ssid=$ONLINE_SSID~ssid=$OFFLINE_SSID~" $HOSTAPD
    done
done

# send hup to hostapd to reload ssid
killall -HUP hostapd
