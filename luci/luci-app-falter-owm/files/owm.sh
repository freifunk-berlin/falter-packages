#!/bin/sh

# This software originates from Freifunk Berlin and registers nodes
# from the Freifunk Berlin Network at our online map at https://openwifimap.net.
# This is a reimplementation of a former lua-script.
# It is licensed under GNU General Public License v3.0 or later
# Copyright (C) 2021   Patrick Grimm
# Copyright (C) 2021   Martin Hübner

# Omit warning for missing local statement. busybox-ash has them included
# shellcheck shell=dash

# jshn assigns the variables for us, but shellcheck doesn't get this.
# shellcheck disable=SC2154
# We use printf for consistency, even though it has no vars.
# shellcheck disable=SC2182
# We've used a hack for embedding a json-string into a json-string with sed. Having '$OLSRCONFIG' in single-quotes is by intention.
# shellcheck disable=SC2016
# using printf with variables and nc didn't work correctly. Thus this hack
# shellcheck disable=SC2059
# by full intention: jshn needs to get numeric values without double-quotes!
# shellcheck disable=SC2086
# The unused variables remain for future use. We intend to use them later on.
# shellcheck disable=SC2034

# we can't acess those libraries at compile-time. Thus ignoring.
# shellcheck source=/dev/null
. /lib/functions.sh
. /usr/share/libubox/jshn.sh

OWM_API_VER="1.0"

printhelp() {
    printf "owm.sh - Tool for registering routers at openwifimap.net\n
Options:
\t--help|-h:\tprint this text

\t--dry-run:\tcheck if owm.lua is working (does not paste any data).
\t\t\tWith this option you can check for errors in your
\t\t\tconfiguration and test the transmission of data to
\t\t\tthe map.\n\n
If invoked without any options, this tool will try to register
your node at the community-map and print the servers response.
To work correctly, this tool will need at least the geo-location
of the node (check correct execution with --dry-run).

To override the server used by this script, set freifunk.community.owm_api.
"
}

# save positional argument, as it would get overwritten otherwise.
CMD_1="$1"
if [ -n "$CMD_1" ] && [ "$CMD_1" != "--dry-run" ]; then
    [ "$CMD_1" != "-h" ] && [ "$CMD_1" != "--help" ] && printf "Unrecognized argument %s.\n\n" "$CMD_1"
    printhelp
    exit 1
fi

# calback function: This function aggregates all items of the 'contact'
# option list from /etc/config/freifunk into one single string for better
# transport
handle_contact() {
    local value="$1"

    if [ -n "$value" ]; then
        CONTACT_AGGREGATOR="$CONTACT_AGGREGATOR|$value"
    fi
}

######################
#                    #
#  Collect OWM-Data  #
#                    #
######################

olsr4_links() {
    json_select "$2"
    json_get_var localIP localIP
    json_get_var remoteIP remoteIP
    # extract the second level domain from the host and append .olsr to be compatible with bgbdisco suffix .ff
    remotehost="$(nslookup "$remoteIP" 2>/dev/null | grep name | sed -e 's/.*name = \(.*\)/\1/' | awk -F. '{print $(NF-1)".olsr"}')"
    if [ -z "$remotehost" ]; then
        remotehost="$remoteIP"
    fi
    json_get_var linkQuality linkQuality
    json_get_var olsrInterface olsrInterface
    json_get_var ifName ifName
    json_select ..
    if ! echo "$olsrInterface" | grep -q -E '.*(wg|ts)_.*'; then
        olsr4links="$olsr4links$localIP $remoteIP $remotehost $linkQuality $ifName;"
    fi
}

# This section is relevant for hopglass statistics feature (isUplink/isHotspot)
OLSRCONFIG=$(printf "/config" | nc 127.0.0.1 9090)

# collect nodes location
uci_load system
longitude="$(uci_get system @system[-1] longitude)"
latitude="$(uci_get system @system[-1] latitude)"

#
#   Stop execution if lat/lon is not set.
#
if [ -z "$latitude" ] || [ -z "$longitude" ]; then
    printf "latitude/longitude is not set.\nStopping now...\n"
    exit 2
fi

# collect data on OLSR-links
json_load "$(printf "/links" | nc 127.0.0.1 9090 2>/dev/null)" 2>/dev/null
#json_get_var timeSinceStartup timeSinceStartup
olsr4links=""
if json_is_a links array; then
    json_for_each_item olsr4_links links
fi
json_cleanup

# collect data on Bird-Babel mesh links
# Babel routes both IPv4 and IPv6, so we extract IPv4 links
# Strategy: Correlate Babel IPv6 neighbors with IPv4 ARP table via MAC addresses
babelinks=""
BIRD_STATUS=$(birdc show status 2>/dev/null)
if [ -n "$BIRD_STATUS" ] && echo "$BIRD_STATUS" | grep -q "Router ID"; then
    # Build IPv4 neighbor table: MAC -> IPv4
    ip -4 neigh show | grep -v FAILED >/tmp/babel_ip_neigh.txt

    # Build IPv6 neighbor table: IPv6 -> MAC
    ip -6 neigh show | grep -v FAILED >/tmp/babel_ip6_neigh.txt

    # Parse Babel interfaces to get local IPv4 per interface
    # Also collect all local IPs for self-link detection
    birdc show babel interfaces 2>/dev/null | tail -n +4 >/tmp/babel_ifaces.txt
    # Get all local IPv4s (column 7) for self-link detection
    LOCAL_IPS=$(awk '{print $7}' /tmp/babel_ifaces.txt | tr '\n' ' ')

    BABEL_OUTPUT=$(birdc show babel neighbors 2>/dev/null)
    if [ -n "$BABEL_OUTPUT" ]; then
        echo "$BABEL_OUTPUT" | tail -n +4 >/tmp/babel_neighbors.txt
        while IFS= read -r line; do
            [ -z "$line" ] && continue

            # Parse: IP Interface Metric Routes Hellos Expires Auth RTT
            neighbor_ipv6=$(echo "$line" | awk '{print $1}')
            ifName=$(echo "$line" | awk '{print $2}')
            metric=$(echo "$line" | awk '{print $3}')

            [ -z "$neighbor_ipv6" ] || [ -z "$ifName" ] || [ -z "$metric" ] && continue
            echo "$neighbor_ipv6" | grep -qE '^fe80::' || continue
            echo "$ifName" | grep -qE '(wg|ts)_' && continue

            # Get local IPv4 for this interface (column 7)
            local_ipv4=$(grep "^${ifName} " /tmp/babel_ifaces.txt 2>/dev/null | awk '{print $7}')
            [ -z "$local_ipv4" ] && continue

            # Look up MAC from IPv6 neighbor table
            neighbor_mac=$(grep "^${neighbor_ipv6} " /tmp/babel_ip6_neigh.txt 2>/dev/null | awk '{print $5}')
            [ -z "$neighbor_mac" ] && continue

            # Look up IPv4 from MAC, filtering by interface name
            neighbor_ipv4=$(grep "${ifName}" /tmp/babel_ip_neigh.txt | grep "${neighbor_mac}" | awk '{print $1}')
            [ -z "$neighbor_ipv4" ] && continue

            # Skip if remote IPv4 is any of our local IPs (self-link)
            for local_ip in $LOCAL_IPS; do
                [ "$neighbor_ipv4" = "$local_ip" ] && continue 2
            done

            # Convert metric to quality: quality = 256 / metric
            if [ "$metric" -gt 0 ] && [ "$metric" -lt 65534 ]; then
                quality=$(awk "BEGIN {printf \"%.3f\", 256 / $metric}")
            else
                quality="0.01"
            fi

            # Get hostname via nslookup, replace .ff with .olsr
            remotehost="$(nslookup "$neighbor_ipv4" 2>/dev/null | grep name | sed -e 's/.*name = \(.*\)/\1/' | awk -F. '{print $(NF-1)".olsr"}')"
            if [ -z "$remotehost" ]; then
                remotehost="$neighbor_ipv4"
            fi

            babelinks="$babelinks$local_ipv4 $neighbor_ipv4 $remotehost $quality $ifName;"
        done </tmp/babel_neighbors.txt
    fi
    rm -f /tmp/babel_ip_neigh.txt /tmp/babel_ip6_neigh.txt /tmp/babel_ifaces.txt /tmp/babel_neighbors.txt
fi

# collect board info
json_load "$(ubus call system board)"
json_get_var model model
json_get_var hostname hostname
json_get_var system system
json_select release
json_get_var revision revision
json_get_var distribution distribution
json_get_var version version
json_select ..
json_load "$(ubus call system info)"
json_get_var uptime uptime
json_get_values loads load

# if file freifunk_release is available, override version and revision
if [ -f /etc/freifunk_release ]; then
    . /etc/freifunk_release
    distribution="$FREIFUNK_DISTRIB_ID"
    version="$FREIFUNK_RELEASE"
    revision="$FREIFUNK_REVISION"
fi

# Get Sysload
sysload=$(cat /proc/loadavg)
load1=$(echo "$sysload" | cut -d' ' -f1)
load5=$(echo "$sysload" | cut -d' ' -f2)
load15=$(echo "$sysload" | cut -d' ' -f3)

# Date when the firmware was build.
kernelString=$(cat /proc/version)
buildDate=$(echo "$kernelString" | cut -d'#' -f2 | cut -c 3-)
kernelVersion=$(echo "$kernelString" | cut -d' ' -f3)

# contact information
uci_load freifunk
name="$(uci_get freifunk contact name)"
nick="$(uci_get freifunk contact nickname)"
mail="$(uci_get freifunk contact mail)"
phone="$(uci_get freifunk contact phone)"
homepage="$(uci_get freifunk contact homepage)" # whitespace-separated, with single quotes, if string contains whitspace
note="$(uci_get freifunk contact note)"

# aggregate contacts-list into one string
config_load freifunk
config_list_foreach contact contact handle_contact
# omit the first pipe-symbol.
contacts=$(echo "$CONTACT_AGGREGATOR" | sed 's/|//')

# community info
ssid="$(uci_get freifunk community ssid)"
mesh_network="$(uci_get freifunk community mesh_network)"
uci_owm_api="$(uci_get freifunk community owm_api)"
com_name="$(uci_get freifunk community name)"
com_homepage="$(uci_get freifunk community homepage)"
com_longitude="$(uci_get freifunk community longitude)"
com_latitude="$(uci_get freifunk community latitude)"
com_ssid_scheme=$(uci_get freifunk community ssid_scheme)
com_splash_network=$(uci_get freifunk community splash_network)
com_splash_prefix=$(uci_get freifunk community splash_prefix)

###########################
#                         #
#  Construct JSON-string  #
#                         #
###########################

json_init
json_add_object freifunk
{
    json_add_object contact
    {
        if [ -n "$name" ]; then json_add_string name "$name"; fi
        # contact list superseeds the use of mail option
        if [ -n "$contacts" ]; then
            json_add_string mail "$contacts"
        else
            if [ -n "$mail" ]; then json_add_string mail "$mail"; fi
        fi
        if [ -n "$nick" ]; then json_add_string nickname "$nick"; fi
        if [ -n "$phone" ]; then json_add_string phone "$phone"; fi
        if [ -n "$homepage" ]; then json_add_string homepage "$homepage"; fi # was array of homepages
        if [ -n "$note" ]; then json_add_string note "$note"; fi
    }
    json_close_object

    json_add_object community
    {
        json_add_string ssid "$ssid"
        json_add_string mesh_network "$mesh_network"
        json_add_string owm_api "$uci_owm_api"
        json_add_string name "$com_name"
        json_add_string homepage "$com_homepage"
        json_add_string longitude "$com_longitude"
        json_add_string latitude "$com_latitude"
        json_add_string ssid_scheme "$com_ssid_scheme"
        json_add_string splash_network "$com_splash_network"
        json_add_int splash_prefix $com_splash_prefix
    }
    json_close_object
}
json_close_object

# script infos
json_add_string type "node"
json_add_string script "owm.sh"
json_add_double api_rev $OWM_API_VER

json_add_object system
{
    json_add_array sysinfo
    {
        json_add_string "" "system is deprecated"
        json_add_string "" "$model"
    }
    json_close_array
    json_add_array uptime
    {
        json_add_int "" $uptime
    }
    json_close_array
    json_add_array loadavg
    {
        json_add_double "" $load5
    }
    json_close_array
}
json_close_object

# OLSR-Config
# That string gets substituted by the olsrd-config-string afterwards
json_add_object olsr
{
    json_add_string ipv4Config '$OLSRCONFIG'
}
json_close_object

json_add_array links
{
    IFSORIG="$IFS"
    IFS=';'
    for i in ${babelinks}; do
        IFS="$IFSORIG"
        set -- $i
        json_add_object
        {
            json_add_string sourceAddr4 "$1"
            json_add_string destAddr4 "$2"
            json_add_string id "$3"
            json_add_double quality "$4"
        }
        json_close_object
        IFS=';'
    done
    for i in ${olsr4links}; do
        IFS="$IFSORIG"
        set -- $i
        json_add_object
        {
            json_add_string sourceAddr4 "$1"
            json_add_string destAddr4 "$2"
            json_add_string id "$3"
            json_add_double quality "$4"
        }
        json_close_object
        IFS=';'
    done
    IFS="$IFSORIG"
}
json_close_array

# General node info
# Bug in add_double function. Mostly it adds unwanted digits
# but they disappear, if we send stuff to the server
json_add_double latitude $latitude
json_add_double longitude $longitude
json_add_string hostname "$hostname"
json_add_int updateInterval 1800
json_add_string hardware "$system"
json_add_object firmware
{
    json_add_string name "$distribution $version"
    json_add_string revision "$revision"
    json_add_string kernelVersion "$kernelVersion"
    json_add_string kernelBuildDate "$buildDate"
}
json_close_object

json_close_object

JSON_STRING=$(json_dump)
# insert json-string from OLSR and repair wrong syntax at string-borders (shell-quotes...)
JSON_STRING=$(echo "$JSON_STRING" | sed -e 's|$OLSRCONFIG|'"$OLSRCONFIG"'|; s|"{|{|; s|}"|}|')

# just print data to stdout, if we have test-run.
if [ "$CMD_1" = "--dry-run" ]; then
    printf "%s\n" "$JSON_STRING"
    exit 0
fi

################################
#                              #
#   Send data to openwifimap   #
#                              #
################################

LEN=${#JSON_STRING}

MSG="\
PUT /update_node/$hostname.olsr HTTP/1.1\r
User-Agent: nc/0.0.1\r
Host: api.openwifimap.net\r
Content-type: application/json\r
Content-length: $LEN\r
\r
$JSON_STRING\r\n"

server="api.openwifimap.net"
server_ips="$(nslookup -type=AAAA $server 2>/dev/null | grep 'Address' | grep -v '127.0.0.1' | grep ':' | awk '{print $NF}')"
server_ips="$server_ips $(nslookup -type=A $server 2>/dev/null | grep 'Address' | grep -v '127.0.0.1' | awk '{print $NF}')"

if [ -n "$server_ips" ]; then
    for server_ip in $server_ips; do
        printf "Try Server IP: $server_ip "
        if printf "$MSG" | nc $server_ip 80 >/tmp/owm_server_ret 2>&1; then
            printf "OK\n"
            break
        else
            printf "Fail\n"
        fi
    done
else
    printf "Fail nslookup $server\n"
fi
