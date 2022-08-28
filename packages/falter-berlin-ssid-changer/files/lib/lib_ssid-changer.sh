#/bin/sh

# shellcheck shell=dash
# shellcheck disable=SC2155

. /lib/functions.sh
. /lib/functions/network.sh

DNS_SERVER=$(uci_get network loopback dns)
NODENAME=$(uci_get system @system[-1] hostname | cut -b -24) # cut nodename to not exceed 32 bytes
export OFFLINE_SSID="offline_""$NODENAME"

log() {
    logger -s -t "ssid-changer" -p 5 "$1"
}

increment_ip_addr() {
    local raw_addr="$1"

    local net=$(echo "$raw_addr" | cut -d'.' -f -3)
    local host=$(echo "$raw_addr" | cut -d'.' -f 4)
    host=$((host + 1))
    echo "$net"".$host"
}

is_internet_reachable_via_ffuplink() {
    # check if clients could reach the internet via ffuplink (only exists if we have a wan port)
    for SERVER in $DNS_SERVER; do
        if ping -c1 -W 3 -I ffuplink "$SERVER" >/dev/null; then
            return 0
        fi
    done
    return 1
}

exists_route_over_mesh() {
    # if internet not reachable via ffuplink, is there no route over the mesh either?
    local dhcp
    network_get_ipaddr dhcp dhcp
    dhcp=$(increment_ip_addr "$dhcp")
    for SERVER in $DNS_SERVER; do
        if ip r g "$SERVER" from "$dhcp" iif br-dhcp; then # theres a route to the internet.
            return 0
        fi
    done
    return 1
}

is_internet_reachable() {
    is_internet_reachable_via_ffuplink
    if [ $? = 1 ]; then
        exists_route_over_mesh
        if [ $? = 1 ]; then
            return 1
        fi
    fi
    return 0
}

get_interfaces() {
    # get the names of every interface named *dhcp* and fetch its normal ssid. Thus we get
    # 2.4 and/or 5 GHz both
    local IFACES=$(uci show wireless | grep -e "dhcp.*\.ssid='.*\.freifunk.net'" | cut -d'=' -f1)
    local ONLINE_SSIDS=""
    for IFACE in $IFACES; do
        local SSID=$(uci_get "$IFACE")
        ONLINE_SSIDS="$SSID $ONLINE_SSIDS"
    done
    echo "$ONLINE_SSIDS"
}
