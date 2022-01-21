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

cleanup() {
    log "Closing"
    exit
}
trap cleanup EXIT

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
    return $(($(date +%s) - $(wg show "$interface" latest-handshakes | awk '{print $2}')))
}

teardown() {
    local interface="$1"
    # ToDo: down.sh should be dynamic...
    down.sh "$interface"
    ip link delete dev "$interface"
}

wg_get_usage() {
    local server="$1"
    # ToDo: PASSWORDS!!!!11!!111!!
    clients=$(wg-client-installer get_usage --endpoint "$server" --user wginstaller --password wginstaller)
    return "$(echo "$clients" | cut -d' ' -f2)"
}

get_least_used_tunnelserver() {
    local tunnel_endpoints="$1"
    local connections="$2"

    # Dont check tunnelserver we already have a connection with
    for i in $connections; do
        ip=$(wg show "$i" endpoints | awk -F'[\t:]' '{print $2}')
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

newtunnel() {
    local ip="$1"
    local nsname="$1"

    # If there isn't a proper key-pair, generate it
    [ -d "/tmp/run/wgclient" ] || mkdir -p /tmp/run/wgclient
    gw_key="/tmp/run/wgclient/wg.key"
    gw_pub="/tmp/run/wgclient/wg.pub"
    if [ ! -f $gw_key ] || [ ! -f $gw_pub ]; then
        log "No proper keys found. Generating a new pair of keys..."
        rm -f $gw_key $gw_pub
        wg genkey | tee $gw_key | wg pubkey >$gw_pub
        log "generation done."
    fi

    interface=$(timeout 5 ip netns exec uplink wg-client-installer register --endpoint "$ip" --user wginstaller --password wginstaller --wg-key-file $gw_pub --mtu 1412)
    log "New tunnel interface is $interface"

    # move WG interface to default namespace to allow meshing on it
    ip link set dev "$interface" netns 1
    post_setup.sh "$interface"
}

# This method sets up the Tunnels and ensures everything is up and running
manage() {
    local nsname="$1"
    local connection_count="$2"
    local tunnel_endpoints="$3"
    local intervall=60
    local tunneltimeout=600

    # Connections holds a list of current WG interfaces (e.g wg_51312 )
    connections=$(ip link | grep ' wg_[0-9]*:' | awk '{print $2}' | sed 's|:||')
    log "current connections: $connections"

    # Check for stale tunnels and tear em down
    for conn in $connections; do
        get_age "$conn"
        age=$?
        if [ $age -ge $tunneltimeout ]; then
            log "Tunnel to $conn timed out. Try to recreate it."
            teardown "$conn"

            #ToDo: currently only one tunnel.
            ep=$(get_least_used_tunnelserver "$tunnel_endpoints" "$connections")
            log "Server handling least clients is: $ep. Trying to create tunnel..."
            new_tunnel "$ep" "$nsname"
            connections=$(ip link | grep ' wg_[0-9]*:' | awk '{print $2}' | sed 's|:||')
            # Setup new Tunnels until we have enough
            #while connections | wc -w <$connection_count; do
            #    ep=get_least_used_tunnelserver $tunnel_endpoints $connections
            #    new_tunnel $ep $nsname
            #    connections=$connections+$tunnel
            #
            #    # Sleep to not overwhelm the cpu :)
            #    sleep $intervall
            #done
        fi
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

setup_namespace "$OPT_NAMESPACE_NAME" "$OPT_UPLINK_INTERFACE" "$OPT_UPLINK_IP" "$OPT_UPLINK_GW"

manage "$OPT_NAMESPACE_NAME" "$OPT_TUNNEL_COUNT" "$OPT_TUNNEL_ENDPOINTS"
