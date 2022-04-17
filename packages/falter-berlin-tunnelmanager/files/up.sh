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

cleanup_olsr_wg_config() {
    # all_olsr_interfaces=$(echo "/int" | nc 127.0.0.1 2006 | tail +3 | awk '{print $1}')
    olsr_wg_interfaces=$(echo "/int" | nc 127.0.0.1 2006 | grep wg_ | awk '{print $1}')

    i=0
    while uci get olsrd.@Interface[$i] &>/dev/null; do
        int_name=$(uci get olsrd.@Interface[$i].interface)
        if [ $? -ne 0 ]; then # check if interface config is wrong
            uci delete olsrd.@Interface[$i]
            continue
        fi

        # skip non wireguard interfaces
        slicedint=$(echo $int_name | cut -c1-3)
        if [ "${slicedint}" != "wg_" ]; then
            i=$((i + 1))
            continue
        fi

        # shell ...
        do_delete=1
        for olsr_int in $olsr_wg_interfaces; do
            if [ "$int_name" = "$olsr_int" ]; then
                do_delete=0
            fi
        done

        if [ $do_delete -eq 1 ]; then
            uci delete olsrd.@Interface[$i]
        else
            i=$((i + 1))
        fi
    done
    uci commit
}

cleanup_babel_wg_config() {
    # all_babel_interfaces=$(echo "dump" | nc ::1 33123 | grep interface | awk '{print $3}')
    babel_wg_interfaces=$(echo "dump" | nc ::1 33123 | grep interface | grep wg_ | awk '{print $3}')

    i=0
    while uci get babeld.@interface[$i] &>/dev/null; do
        int_name=$(uci get babeld.@interface[$i].ifname)
        if [ $? -ne 0 ]; then # check if interface config is wrong
            uci delete babeld.@interface[$i]
            continue
        fi

        # skip non wireguard interfaces
        slicedint=$(echo $int_name | cut -c1-3)
        if [ "${slicedint}" != "wg_" ]; then
            i=$((i + 1))
            continue
        fi

        # shell ...
        do_delete=1
        for babel_int in $babel_wg_interfaces; do
            if [ "$int_name" = "$babel_int" ]; then
                do_delete=0
            fi
        done

        if [ $do_delete -eq 1 ]; then
            uci delete babeld.@interface[$i]
        else
            i=$((i + 1))
        fi
    done
    uci commit
}

get_ips_from_prefix "$prefix"

# unconditionally wipe all configured ips from available ips.
available_ips="$_ret_prefixes"
for i in $(get_configured_ips); do
    available_ips="$(echo $available_ips | sed "s/\b$i\b//")"
done

next_ip="$(echo $available_ips | awk '{print $1}')"

# Configure IPs
ip address add "$next_ip/32" dev "$interface"
ip address add "fe80::2/64" dev "$interface"

# bringup interface
ip link set up dev "$interface"

cleanup_olsr_wg_config
cleanup_babel_wg_config

# wait some time before bringing up olsrd and babeld
sleep 2

# Configure OLSRD
uci revert olsrd

UCIREF="$(uci add olsrd Interface)"
uci set "olsrd.$UCIREF.ignore=0"
uci set "olsrd.$UCIREF.interface=$interface"
uci set "olsrd.$UCIREF.Mode=ether"
uci commit olsrd

# check if olsrd is started and ubus is available
ubus list | grep -qF olsrd
if [ $? -eq 1 ]; then
    # no olsr running start
    /etc/init.d/olsrd start
else
    # instead of reloading add interface via ipc to make it seamless
    ubus call olsrd add_interface '{"ifname":'\""$interface"\"'}'
fi

# Configure babeld
uci revert babeld
UCIREF="$(uci add babeld interface)"
uci set "babeld.$UCIREF.ifname=$interface"
uci set "babeld.$UCIREF.split_horizon=true"
uci commit babeld

# check if babeld is started and ubus is available
ubus list | grep -qF babeld
if [ $? -eq 1 ]; then
    /etc/init.d/babeld start
else
    # instead of reloading add interface via ipc to make it seamless
    ubus call babeld add_interface '{"ifname":'\""$interface"\"'}'
fi
