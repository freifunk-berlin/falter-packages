#!/bin/sh

interface="$1"

# can be controlled using tunnelmanagers "-A" parameter
prefix="$2"

get_ips_from_prefix() {
    local prefix="$1"
    local amount="$(owipcalc "$prefix" howmany 32)"
    local command="owipcalc "$prefix" network print"
    for i in $(seq 2 "$amount"); do
        command="$command next 32 print add 1"
    done
    _ret_prefixes=$($command)
}

get_configured_ips() {
    ip -o -4 a s | awk -F ' |\/' '{print $7}'
}

get_ips_from_prefix "$prefix"


# unconditionally wipe all configured ips from available ips.
available_ips="$_ret_prefixes"
for i in $(get_configured_ips); do
    available_ips="$(echo $available_ips | sed "s/$i//")"
done

next_ip="$(echo $available_ips | awk '{print $1}')"

# Configure IPs
ip address add "$next_ip/32" dev "$interface"
ip address add "fe80::2/64" dev "$interface"

# bringup interface

ip link set up dev "$interface"

# Configure OLSRD
uci revert olsrd

UCIREF="$(uci add olsrd Interface)"
uci set "olsrd.$UCIREF.ignore=0"
uci set "olsrd.$UCIREF.interface=$interface"
uci set "olsrd.$UCIREF.Mode=ether"
uci commit olsrd

/etc/init.d/olsrd reload


# Configure babeld
uci revert babeld
UCIREF="$(uci add babeld interface)"
uci set "babeld.$UCIREF.ifname=$interface"
uci set "babeld.$UCIREF.split_horizon=true"
uci commit babeld

/etc/init.d/babeld reload
