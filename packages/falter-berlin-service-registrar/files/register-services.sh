#!/bin/sh
# shellcheck shell=dash
# shellcheck disable=SC2155

# This software originates from Freifunk Berlin and simplyfies registering
# services in OLSR. It is licensed under GNU General Public License v3.0 or later
# Copyright (C) 2022   Martin Hübner

# shellcheck disable=SC2181
# shellcheck source=/dev/null
. /lib/functions.sh

# ToDo: Website descriptions must not contain a comma!


log() {
    local msg="$1"
    logger -t "ffserviced" -s "$msg"
}

# busybox doesn't include the rev-command. Thus implementing it by ourself.
rev() {
    while read -r var; do
        # function taken from https://stackoverflow.com/a/34668251
        rev=""
        i=1

        while [ "$i" -le "${#var}" ]; do
            rev="$(echo "$var" | awk -v i="$i" '{print(substr($0,i,1))}')$rev"
            : $((i += 1))
        done

        echo "$rev"
    done
}

get_olsrd_nsplugin_index() {
    # get anonymous section number of nameservice_plugin in olsrd-config
    local ipversion="$1"

    if [ "$ipversion" = 4 ]; then
        uci show olsrd | grep nameservice | sed -e 's|.*\(\d\).*|\1|g'
    elif [ "$ipversion" = 6 ]; then
        uci show olsrd6 | grep nameservice | sed -e 's|.*\(\d\).*|\1|g'
    else
        log "cannot find olsrd-conf for IP version $ipversion."
    fi
}

flush_nondefault_uhttpd_sections() {
    local section_name="$1"

    if [ "$section_name" != "main" ] && [ "$section_name" != "defaults" ]; then
        # flush section
        uci_remove uhttpd "$section_name"
    fi
}

flush_olsrd_services() {
    nsplugin=$(get_olsrd_nsplugin_index 4)

    uci_remove olsrd @LoadPlugin["$nsplugin"] hosts
    uci_remove olsrd @LoadPlugin["$nsplugin"] service
}

# This script will empty all entries made in the olsrd nameservice plugin
# So only those entries will remain, that are handled by this script,
flush_old_entries() {
    config_load uhttpd
    config_foreach flush_nondefault_uhttpd_sections

    flush_olsrd_services
}

create_website_template() {
    local path="$1"

    mkdir -p "$path" || return 1
    if [ ! -f "$path"/index.html ]; then
        cat <<EOF >"$path"/index.html
<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <title>It works!</title>
  </head>
  <body>
    <h1>It works!</h1>
    <p>You've successfully registered your static web page as a service in the Freifunk Berlin network. You may replace this example page with your own static website now.</p>
    <p>This file is located under the service root directory at $path/index.html</p>
  </body>
</html>
EOF
        if [ $? != 0 ]; then
            return 1
        fi
    fi
}

register_website_at_uhttpd() {
    # configure website in uhttpd
    local uci_section="$1"
    local fqdn="$2"
    local port="$3"
    local web_root="$4"

    uci_add uhttpd uhttpd "$uci_section"
    uci_add_list uhttpd "$uci_section" listen_http "0.0.0.0:$port"
    uci_add_list uhttpd "$uci_section" listen_http "[::]:$port"
    # ToDo: Test, that uci handles "/tmp/www/website//" correctly
    uci_set uhttpd "$uci_section" home "$web_root/"
    uci_set uhttpd "$uci_section" max_requests 5
    uci_set uhttpd "$uci_section" max_connections 100
}

register_service_at_olsrd() {
    local addr="$1"
    local fqdn="$2"
    local port="$3"
    local protocol="$4"
    local description="$5"
    # ToDo: service.name.olsr should work too, need to work with reverse...
    local hostname=$(echo "$fqdn" | rev | cut -d'.' -f 2- | rev | tr '.' '-')

    host_announcment="$addr $hostname"
    service_announcement="http://$fqdn:$port|$protocol|$description"

    # get anonymous section number of nameservice_plugin in olsrd-config
    nsplugin=$(get_olsrd_nsplugin_index 4)

    # generate service entry in olsrd-config
    uci_add_list olsrd @LoadPlugin["$nsplugin"] hosts "$host_announcment"
    uci_add_list olsrd @LoadPlugin["$nsplugin"] service "$service_announcement"
}

apply_configs() {
    uci commit uhttpd
    uci_commit olsrd
    /etc/init.d/uhttpd restart
    /etc/init.d/olsrd restart
    /etc/init.d/dnsmasq restart
}

handle_service() {
    local uci_section="$1"

    # 1. Name of a variable to store the retrieved value in
    # 2. ID of the section to read the value from
    # 3. Name of the option to read the value from
    # 4. Default (optional), value to return instead if option is unset
    # see https://openwrt.org/docs/guide-developer/config-scripting

    config_get fqdn "$uci_section" fqdn
    config_get description "$uci_section" description
    config_get protocol "$uci_section" protocol "tcp"
    config_get port "$uci_section" port "80"
    config_get ip_addr "$uci_section" ip_addr
    config_get disabled "$uci_section" disabled "0"

    if [ "${disabled:?}" = 0 ]; then

        if [ -z "$fqdn" ] || [ -z "$description" ] || [ -z "$ip_addr" ]; then
            log "service configuration failed! UCI section $uci_section misses at least one of mandatory fqdn, description or ip_addr options"
            # return 0 to allow configuration of other services
            return 0
        fi

        log "registering service with
    FQDN.................: $fqdn
    Name/Description.....: $description
    Protocol.............: $protocol
    Port.................: $port
    IP Address...........: $ip_addr"

        register_service_at_olsrd "$ip_addr" "$fqdn" "$port" "$protocol" "$description"

    fi
}

handle_website() {
    local uci_section="$1"

    # see https://openwrt.org/docs/guide-developer/config-scripting

    config_get fqdn "$uci_section" fqdn
    config_get description "$uci_section" description
    config_get protocol "$uci_section" protocol "tcp"
    config_get port "$uci_section" port "80"
    config_get web_root "$uci_section" web_root
    config_get disabled "$uci_section" disabled "0"

    if [ "$disabled" = 0 ]; then

        if [ -z "$fqdn" ] || [ -z "$description" ] || [ -z "$web_root" ]; then
            log "service configuration failed! UCI section $uci_section misses at least one of mandatory fqdn, description or web_root options"
            # return 0 to allow configuration of other services
            return 0
        fi

        log "registering website with
    FQDN.................: $fqdn
    Name/Description.....: $description
    Protocol.............: $protocol
    Port.................: $port
    Web root.............: $web_root"

        create_website_template "$web_root"

        register_website_at_uhttpd "$uci_section" "$fqdn" "$port" "$web_root"

        website_addr=$(uci_get network dhcp ipaddr) # first IP addr of DHCP subnet
        register_service_at_olsrd "$website_addr" "$fqdn" "$port" "$protocol" "$description"

    fi

}

##########
#  Main  #
##########

# check for config file
if [ ! -f /etc/config/ffservices ]; then
    echo "No configuration file found. Please define it under /etc/config/ffservices."
    exit 2
fi

if [ -z "$(uci show ffservices | sed -e '/^#/d' )" ]; then
    echo "/etc/config/ffservices seems to contain no configuration."
    exit 2
fi

flush_old_entries

config_load ffservices

config_foreach handle_service service
config_foreach handle_website website

apply_configs
