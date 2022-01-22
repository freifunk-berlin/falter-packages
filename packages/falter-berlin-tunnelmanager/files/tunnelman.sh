#! /bin/sh

# except than noted, this script is not posix-compliant in one way: we use "local"
# variables definition. As nearly all shells out there implement local, this should
# work anyway. This is a little reminder to you, if you use some rare shell without
# a builtin "local" statement.


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
    -n: namespace name where uplink shall reside in
    -T: tunnel endpoint (can be used multiple times)
    -c: tunnel_count (how many redundant tunnells)
    -t: tunnel_timeout (seconds, after what time consider tunnels to be down - last handshake)
    -o: interval (how often does the loop run, clean up and establish new connection if needed)
    -D: Pre-Down Script (use too hook after connection is down. - Interface name is passed as \$1)
    -U: Post-Up Script (use too hook after connection is established - Interface name is passed as \$1)
    -E: Post-Up Script Arguments (use to pass parameters to post-up script \$2)

Example call:
    tunnelman.sh -i br-vpn_lte -a 192.168.178.100/24 -g 192.168.178.1 -n vpn_lte -T 176.74.57.43 -T 176.74.57.19 -T 77.87.51.11 -T 77.87.49.8 -c
 1 -t 180 -o 60 -D down.sh -U up.sh -A 10.31.147.224/29
\n"
}

cleanup() {
    for i in $interfaces; do
        teardown "$i"
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
    local endpoint="$(wg show $interface endpoints | awk -F '\t|:' '{print $2}')"

    sh "$OPT_DOWN_SCRIPT" "$interface"
    ip link delete "$interface"


    interfaces=$(echo "$interfaces" | sed "s/ $interface//")
    connections=$(echo "$connections" | sed "s/ $endpoint//")
}

wg_get_usage() {
    local server="$1"
    # ToDo: PASSWORDS!!!!11!!111!!
    clients=$(timeout 5 ip netns exec $OPT_NAMESPACE_NAME wg-client-installer get_usage --endpoint "$server" --user wginstaller --password wginstaller)
    if [ $? -ne 0 ]; then
        return 1
    fi
    echo "$(echo "$clients" | cut -d' ' -f2)"
}

get_least_used_tunnelserver() {
    local tunnel_endpoints="$1"

    # Dont check tunnelserver we already have a connection with
    for i in $(wg show all endpoints | awk -F '\t|:' '{print $3}'); do
        # remove ip from connections:
        tunnel_endpoints=$(echo "$tunnel_endpoints" | sed "s/$i//")
    done

    # Select next best tunnelserver
    best=""
    usercount=99999

    for i in $tunnel_endpoints; do
        current=$(wg_get_usage "$i")
        if [ $? -ne 0 ]; then
            log "Error while querying tunnelserver $i for utilization"

        elif [ $current -le $usercount ]; then
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
    gw_pub="/etc/wireguard/wg.pub"
    if [ ! -f $gw_key ] || [ ! -f $gw_pub ]; then
        log "No proper keys found. Generating a new pair of keys..."
        rm -f $gw_key $gw_pub
        wg genkey | tee $gw_key | wg pubkey >$gw_pub
        log "generation done."
    fi
}


new_tunnel() {
    local ip="$1"
    local nsname="$2"


    local interface=$(timeout 5 ip netns exec $OPT_NAMESPACE_NAME wg-client-installer register --endpoint "$ip" --user wginstaller --password wginstaller --wg-key-file $gw_pub --mtu 1412)

    if [ $? -eq 0 ]; then
        log "New tunnel interface is $interface"

        # move WG interface to default namespace to allow meshing on it
        ip -n "$nsname" link set dev "$interface" netns 1

	interfaces="$interfaces $interface"
	connections="$connections $ip"

        sh "$OPT_UP_SCRIPT" "$interface" "$OPT_UP_SCRIPT_ARGS"
    fi
}

# This method sets up the Tunnels and ensures everything is up and running
manage() {
    local nsname="$1"
    local connection_count="$2"
    local tunnel_endpoints="$3"
    local interval=60

    # Check for stale tunnels and tear em down
    while true; do
        for interface in $interfaces; do
	    if [ $(get_age "$interface") -ge $OPT_TUNNEL_TIMEOUT ]; then
                log "Tunnel to $interface timed out."
                teardown "$interface"
	    fi
	done

	if [ $(echo $connections | wc -w) -lt $connection_count ]; then
            ep=$(get_least_used_tunnelserver "$tunnel_endpoints")
	    if [ ! -z "$ep" ]; then
                log "Server handling least clients is: $ep. Trying to create tunnel..."
		new_tunnel "$ep" "$nsname"
	    else
                log "No servers available..."
	    fi

	fi
        sleep "$interval" %
        wait $!
    done
}

#####################
#   Main Programm   #
#####################

########################
#  Commandline parsing

ENDPOINT_COUNT=0

while getopts a:c:g:i:n:o:t:T:D:U:A: option; do
    case $option in
    a) OPT_UPLINK_IP=$OPTARG ;;
    c) OPT_TUNNEL_COUNT=$OPTARG ;;
    g) OPT_UPLINK_GW=$OPTARG ;;
    i) OPT_UPLINK_INTERFACE=$OPTARG ;;
    n) OPT_NAMESPACE_NAME=$OPTARG ;;
    o) OPT_INTERVAL=$OPTARG ;;
    t) OPT_TUNNEL_TIMEOUT=$OPTARG ;;
    D) OPT_DOWN_SCRIPT=$OPTARG ;;
    U) OPT_UP_SCRIPT=$OPTARG ;;
    A) OPT_UP_SCRIPT_ARGS=$OPTARG ;;
    T)
        if [ $ENDPOINT_COUNT = 0 ]; then
            OPT_TUNNEL_ENDPOINTS=$OPTARG
            ENDPOINT_COUNT=$((ENDPOINT_COUNT + 1))
        else
            OPT_TUNNEL_ENDPOINTS="$OPT_TUNNEL_ENDPOINTS $OPTARG"
            ENDPOINT_COUNT=$((ENDPOINT_COUNT + 1))
        fi
        ;;
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
    [ -z "$OPT_DOWN_SCRIPT" ] || [ -z "$OPT_TUNNEL_TIMEOUT" ]; then
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
    Tunnel-Timeout.......: $OPT_TUNNEL_TIMEOUT
    Work-Interval........: $OPT_INTERVAL
    Up_Script............: $OPT_UP_SCRIPT
    Up_Script-Args.......: $OPT_UP_SCRIPT_ARGS
    Down_Script..........: $OPT_DOWN_SCRIPT"

###############################
#   configure wireguard-stuff


generate_keys

trap cleanup INT TERM
setup_namespace "$OPT_NAMESPACE_NAME" "$OPT_UPLINK_INTERFACE" "$OPT_UPLINK_IP" "$OPT_UPLINK_GW"

# contains list of connected endpoint
connections=""

# contains list of managed wg interfaces
interfaces=""
manage "$OPT_NAMESPACE_NAME" "$OPT_TUNNEL_COUNT" "$OPT_TUNNEL_ENDPOINTS"
