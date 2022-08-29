#!/bin/sh

# shellcheck shell=dash

# in most cases this directive alarms, we don't care for the return values, as we do other checks.
# shellcheck disable=SC2155
# in that case it doesn't work to do it another way. We need that value twice.
# shellcheck disable=SC2181

interface="$1"

# can be controlled using tunnelmanagers "-A" parameter
extra_args="$2"

prefix=$(echo "$extra_args" | cut -d ' ' -f1)

argc=$(echo "$extra_args" | wc -w)
if [ "$argc" -gt "2" ]; then
    metric=$(echo "$extra_args" | cut -d ' ' -f2)
    lqm=$(echo "$extra_args" | cut -d ' ' -f3)
fi

get_ips_from_prefix() {
    local prefix="$1"
    local amount="$(owipcalc "$prefix" howmany 32)"
    local command="owipcalc $prefix network print"
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
        slicedint=$(echo "$int_name" | cut -c1-3)
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
        slicedint=$(echo "$int_name" | cut -c1-3)
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
    available_ips="$(echo "$available_ips" | sed "s/\b$i\b//")"
done

next_ip="$(echo "$available_ips" | awk '{print $1}')"

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
if [ -n "$lqm" ]; then
    uci set "olsrd.$UCIREF.LinkQualityMult=default $lqm"
fi
uci commit olsrd

# check if olsrd is started and ubus is available
ubus list | grep -qF olsrd
if [ $? -eq 1 ]; then
    # no olsr running start
    /etc/init.d/olsrd start
else
    # instead of reloading add interface via ipc to make it seamless
    if [ -n "$lqm" ]; then
        ubus call olsrd add_interface '{"ifname":'\""$interface"\"',"lqm":'\""$lqm"\"'}'
    else
        ubus call olsrd add_interface '{"ifname":'\""$interface"\"'}'
    fi
fi

# Configure babeld
uci revert babeld
UCIREF="$(uci add babeld interface)"
uci set "babeld.$UCIREF.ifname=$interface"
uci set "babeld.$UCIREF.split_horizon=true"
uci commit babeld

if [ -n "$metric" ]; then
    uci revert babeld
    UCIREF="$(uci add babeld filter)"
    uci set "babeld.$UCIREF.type=in"
    uci set "babeld.$UCIREF.if=$interface"
    uci set "babeld.$UCIREF.ip=::/0"
    uci set "babeld.$UCIREF.eq=0"
    uci set "babeld.$UCIREF.action=metric $metric"
    uci commit babeld
fi

# check if babeld is started and ubus is available
ubus list | grep -qF babeld
if [ $? -eq 1 ]; then
    /etc/init.d/babeld start
else
    # instead of reloading add interface via ipc to make it seamless
    ubus call babeld add_interface '{"ifname":'\""$interface"\"'}'
    if [ -n "$metric" ]; then
        ubus call babeld add_filter '{"ifname":'\""$interface"\"',"type":0,"metric":'"$metric"'}'
    fi
fi
