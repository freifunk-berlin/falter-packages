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
    #logger -t vpnmanager -s "$msg"
    # for debugging on local machine
    echo "$msg"
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

cleanup() {
    log "Closing"
    exit
}
trap cleanup EXIT

#
# # This method sets up the Tunnels and ensures everything is up and running
# #manage() {
# #  local nsname="$1"
# #  local connection_count="$2"
# #  local tunnel_endpoints="$3"
# #  local interval=60
# #  local tunneltimeout=600
# #
# #  # Connections holds a list of current WG interfaces (e.g wg_51312 )
# #  connections = ""
# #  while true;
# #
# #  # Check for stale tunnels and tear em down
# #  for conn in connections:
# #  	if get_age $conn > $tunneltimeout:
# #    	teardown $conn
# #
# #      # Setup new Tunnels until we have enough
# #			while true connections | wc -w < connection_count:
# #        $ep= get_least_used_tunnelserver $tunnel_endpoints $connections
# #        new_tunnel $ep $nsname
# #        $connections += $tunnel
# #
# #    		# Sleep to not overwhelm the cpu :)
# #    		sleep $intervall
# #      done
# #    fi
# #	done
# #}
#
#
# get_age(){
# 	local interface="$1"
#   # Check latest handshake, returns value in seconds ago
#   return $(($(date +%s)-$(wg show $interface latest-handshakes | awk '{print $2}')))
# }
#
#
# teardown(){
#   local interface="$1"
#   down.sh $interface
#   ip link delete dev "$interface"
# }
#
#
# #get_least_used_tunnelserver() {
# #  local tunnel_endpoints="$1"
# #  local connections="$2"
# #
# #  # Dont check tunnelserver we already have a connection with
# #
# #  for i in $connections
# #  ip=$(wg show $i endpoints | awk -F'[\t:]' '{print $2}')
# #  # remove ip from connections:
# #  tunnel_endpoints=$(echo \$tunnel_endpoints | sed 's/$ip//')
# #
# #  # Select next best tunnelserver
# #
# #  best = ""
# #  usercount = 99999
# #
# #  for i in $tunnel_endpoints
# #  current = wg_get_usage
# #  if $current < $usercount
# #  best=$i
# #  usercount=$current
# #	return best
# #}

#newtunnel() {
#    local ip="$1"
#    local nsname="$1"
#    $interface = timeout 5 ip netns $nsname exec wg-client-installer $ip
#
#    # move WG interface to default namespace to allow meshing on it
#    ip link set dev $interface netns 1
#    post_setup.sh $interface
#}

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
