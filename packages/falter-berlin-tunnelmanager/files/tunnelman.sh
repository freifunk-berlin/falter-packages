#! /bin/sh

# except than noted, this script is not posix-compliant in one way: we use "local"
# variables definition. As nearly all shells out there implement local, this should
# work anyway. This is a little reminder to you, if you use some rare shell without
# a builtin "local" statement.

# Tunnelman:
# * erstellt Namespace,
# * richtet WAN interface ein und schiebt es in Namespace
# * Stellt sicher das stets $tunnel_count Tunnel aktiv sind (redundanz)
# * Connected neue Tunnelverbindungen und called script nach erfolgreichen Verbindungsaufbau
# * Löscht stale Tunnelverbindungen und called script bei abgeräumten Verbindungsaufbau
#
# * Was fehlt: Logik zum abräumen des Namespaces bei on exit
#
#
# tunnelman (-i eth0.50 -a 192.168.1.2/24 -g 192.168.1.1 -n uplink -T TunnelIP1 -T tunnelip2 -c 2 -t 300 -D down.sh -U up.sh )
#
# Arguments:
# -i : uplink_interface
# -a: uplink_ip
# -g: uplink_gw
# -n:  namespace name
# -T: tunnel endpoint
# -c: tunnel_count
# -t: interval

log() {
    local msg="$1"
    logger -t vpnmanager -s "$msg"
    # for debugging on local machine
    #echo "$msg"
}


print_help() {
    printf "\

Tunnelmanager: Tool for spawning wireguard-tunnels

Arguments:
    -i: uplink_interface
    -a: uplink_ip
    -g: uplink_gw
    -n: namespace name
    -T: tunnel endpoint
    -c: tunnel_count
    -t: interval

Example call:
    tunnelman -i eth0.50 -a 192.168.1.2/24 -g 192.168.1.1 -n uplink -T TunnelIP1 -T tunnelip2 -c 2 -t 300 -D down.sh -U up.sh 
\n"
}

cleanup() {
    for i in $connections; do
        teardown $connection
    done
    ip netns delete "$OPT_NAMESPACE_NAME"
    log "Closing"
    exit
}
setup_namespace() {
    local namespace_name="$1"
    local uplink_interface="$2"
    local uplink_ip="$3"
    local uplink_gw="$4"

    local final_uplink_interface="ul-$namespace_name"

    if ip netns list | grep -q "$namespace_name"; then
        log "Namespace $namespace_name already exists."
        exit 1
    fi

    if ! ip netns add "$namespace_name"; then
        log "Error while setting up namespace $namespace_name"
        exit 1
    fi

    # for now we unconditionally attach a subinterface to the given uplink_interface
    # which is then moved to the namespace. If performance suffers we can implement
    # later a method to directly pass over the physicall interface

    if ! ip link add "$final_uplink_interface" link "$uplink_interface" type macvlan mode bridge; then
        log "Error while setting up macvlan-based uplink interface $final_uplink_interface attached to $uplink_interface"
        exit 1
    fi

    if ! ip link set dev "$final_uplink_interface" netns "$namespace_name"; then
        log "Error while moving uplink interface $final_uplink_interface attached to $uplink_interface"
        exit 1
    fi

    # Bringup interface
    ip -n "$namespace_name" link set up dev "$final_uplink_interface"

    # Configure IP addressing
    ip -n "$namespace_name" address add "$uplink_ip" dev "$final_uplink_interface"
    ip -n "$namespace_name" route add default via "$uplink_gw"

    return 0
}

get_age() {
    local interface="$1"
    # Check latest handshake, returns value in seconds ago
    echo $(($(date +%s) - $(wg show "$interface" latest-handshakes | awk '{print $2}')))
}

teardown() {
    local interface="$1"
    # ToDo: down.sh should be dynamic...
    $OPT_DOWN_SCRIPT "$interface"
    ip link delete dev "$interface"
}

wg_get_usage() {
    local server="$1"
    # ToDo: PASSWORDS!!!!11!!111!!
    clients=$(wg-client-installer get_usage --endpoint "$server" --user wginstaller --password wginstaller)
    echo "$(echo "$clients" | cut -d' ' -f2)"
}

get_least_used_tunnelserver() {
    local tunnel_endpoints="$1"

    # Dont check tunnelserver we already have a connection with
    for i in $(wg show all endpoints); do
        # remove ip from connections:
        tunnel_endpoints=$(echo "$tunnel_endpoints" | sed "s/$ip//")
    done

    # Select next best tunnelserver
    best=""
    usercount=99999

    for i in $tunnel_endpoints; do
        wg_get_usage "$i"
        current=$?
        if [ $current -le $usercount ]; then
            best=$i
            usercount=$current
        fi
    done
    echo "$best"
}


generate_keys() {
    # If there isn't a proper key-pair, generate it
    [ -d "/etc/wireguard" ] || mkdir -p /etc/wireguard
    gw_key="/etc/wireguard/wg.key"
    gw_pub="/etc/wireguard wg.pub"
    if [ ! -f $gw_key ] || [ ! -f $gw_pub ]; then
        log "No proper keys found. Generating a new pair of keys..."
        rm -f $gw_key $gw_pub
        wg genkey | tee $gw_key | wg pubkey >$gw_pub
        log "generation done."
    fi
}


newtunnel() {
    local ip="$1"
    local nsname="$2"


    interface=$(timeout 5 ip netns exec uplink wg-client-installer register --endpoint "$ip" --user wginstaller --password wginstaller --wg-key-file $gw_pub --mtu 1412)
    log "New tunnel interface is $interface"

    # move WG interface to default namespace to allow meshing on it
    ip link set dev "$interface" netns 1
    $OPT_UP_SCRIPT "$interface"
    echo $interface
}

# This method sets up the Tunnels and ensures everything is up and running
manage() {
    local nsname="$1"
    local connection_count="$2"
    local tunnel_endpoints="$3"
    local interval=60
    local tunneltimeout=600

    # Check for stale tunnels and tear em down
    while true; do
        for connection in $connections; do
            if [ get_age "$conn" -ge $tunneltimeout ]; then
                log "Tunnel to $connection timed out."
                teardown "$conn"
	    fi
	done

	if [ $(echo \$connections | wc -w) -lt $connection_count ]; then
            ep=$(get_least_used_tunnelserver "$tunnel_endpoints" "$connections")
	    # todo: error handling if no endpoints available
            log "Server handling least clients is: $ep. Trying to create tunnel..."
	    interface=$(new_tunnel "$ep" "$nsname")
	fi
        sleep $interval
    done
}

#####################
#   Main Programm   #
#####################

########################
#  Commandline parsing

ENDPOINT_COUNT=0

while getopts a:c:g:i:n:t:T:D:U: option; do
    case $option in
    a) OPT_UPLINK_IP=$OPTARG ;;
    c) OPT_TUNNEL_COUNT=$OPTARG ;;
    g) OPT_UPLINK_GW=$OPTARG ;;
    i) OPT_UPLINK_INTERFACE=$OPTARG ;;
    n) OPT_NAMESPACE_NAME=$OPTARG ;;
    t) OPT_INTERVAL=$OPTARG ;;
    D) OPT_UP_SCRIPT=$OPTARG ;;
    T)
        if [ $ENDPOINT_COUNT = 0 ]; then
            OPT_TUNNEL_ENDPOINTS=$OPTARG
            ENDPOINT_COUNT=$((ENDPOINT_COUNT + 1))
        else
            OPT_TUNNEL_ENDPOINTS="$OPT_TUNNEL_ENDPOINTS $OPTARG"
            ENDPOINT_COUNT=$((ENDPOINT_COUNT + 1))
        fi
        ;;
    U) OPT_DOWN_SCRIPT=$OPTARG ;;
    *)
        print_help
        exit 2
        ;;
    esac
done

# check if we got all information necessary
if [ -z "$OPT_UPLINK_IP" ] || [ -z "$OPT_TUNNEL_COUNT" ] ||
    [ -z "$OPT_UPLINK_GW" ] || [ -z "$OPT_UPLINK_INTERFACE" ] ||
    [ -z "$OPT_NAMESPACE_NAME" ] || [ -z "$OPT_INTERVAL" ] ||
    [ -z "$OPT_UP_SCRIPT" ] || [ -z "$OPT_TUNNEL_ENDPOINTS" ] ||
    [ -z "$OPT_DOWN_SCRIPT" ]; then
    printf "Not enough options. Please give all necessary options!\n\n"
    print_help
    exit 2
fi

# TODO: Check arguments for plausability, so that they can't crash the script

log "starting tunnelmanager with
    Uplink-Interface.....: $OPT_UPLINK_INTERFACE
    Uplink-IP............: $OPT_UPLINK_IP
    Uplink-GW............: $OPT_UPLINK_GW
    Namespace............: $OPT_NAMESPACE_NAME 
    Tunnel-Endpoints.....: $OPT_TUNNEL_ENDPOINTS 
    Tunnel-Count.........: $OPT_TUNNEL_COUNT 
    Interval.............: $OPT_INTERVAL
    Up_Script............: $OPT_DOWN_SCRIPT 
    Down_Script..........: $OPT_UP_SCRIPT"

###############################
#   configure wireguard-stuff


generate_keys

trap cleanup EXIT
setup_namespace "$OPT_NAMESPACE_NAME" "$OPT_UPLINK_INTERFACE" "$OPT_UPLINK_IP" "$OPT_UPLINK_GW"

# contains list of connected endpoint
connections=""

# contains list of managed wg interfaces
interfaces=""
manage "$OPT_NAMESPACE_NAME" "$OPT_TUNNEL_COUNT" "$OPT_TUNNEL_ENDPOINTS"
