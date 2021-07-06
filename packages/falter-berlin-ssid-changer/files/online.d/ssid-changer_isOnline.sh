#!/bin/sh
. /lib/functions.sh
. /lib/lib_ssid-changer.sh

# We do assume, that both radios (if there is more than one) share the same
# SSID. Handling different SSIDS is too complicated.

# check, if we really went back online
is_internet_reachable
[ $? = 1 ] && exit


ONLINE_SSIDS=$(get_interfaces)
CHK_SSID=$(echo "$ONLINE_SSIDS" | cut -d' ' -f1) # makes checking easier
LOGMSG="Internet reachable again. Change SSID back to online..."

# abort if SSID is "online" already
for HOSTAPD in $(ls /var/run/hostapd-phy*); do
    CURRSSID=$(grep -e "^ssid=" $HOSTAPD | cut -d'=' -f2)
    if [ "$CURRSSID" = "$CHK_SSID" ]; then
        logger -s -t "ssid-changer" -p 5 "SSID online already. Nothing to change."
        exit 0
    fi
done

# loop over hostapd configs and try to switch any matching ID.
for HOSTAPD in $(ls /var/run/hostapd-phy*); do
    for ONLINE_SSID in $ONLINE_SSIDS; do
        logger -s -t "ssid-changer" -p 5 "$LOGMSG"
        sed -i "s~^ssid=$OFFLINE_SSID~ssid=$ONLINE_SSID~" $HOSTAPD
    done
done

# send hup to hostapd to reload ssid
killall -HUP hostapd
