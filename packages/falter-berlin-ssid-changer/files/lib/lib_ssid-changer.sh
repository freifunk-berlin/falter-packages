#/bin/sh
. /lib/functions.sh

NODENAME=$(uci_get system @system[-1] hostname | cut -b -24 ) # cut nodename to not exceed 32 bytes
OFFLINE_SSID="offline_""$NODENAME"

is_internet_reachable() {
    # check if we really lost internet connection.
    local ERROR=0
    local DNS_SERVER="8.8.8.8 1.1.1.1 9.9.9.9"
    for SERVER in $DNS_SERVER; do
        ping -c1 $SERVER > /dev/null
        if [ $? = 0 ]; then
            return 0
        fi
    done
    return 1
}

get_interfaces() {
    # get the names of every interface named *dhcp* and fetch its normal ssid. Thus we get
    # 2.4 and/or 5 GHz both
    local IFACES=$(uci show wireless | grep -e "dhcp.*\.ssid='.*\.freifunk.net'" | cut -d'=' -f1 )
    local ONLINE_SSIDS=""
    for IFACE in $IFACES; do
        local SSID=$(uci_get "$IFACE")
        ONLINE_SSIDS="$SSID $ONLINE_SSIDS"
    done
    echo "$ONLINE_SSIDS"
}
