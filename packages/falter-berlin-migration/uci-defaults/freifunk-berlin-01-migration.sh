#!/usr/bin/env ash
# shellcheck shell=dash

# shellcheck disable=SC1091
# shellcheck disable=SC3043
# shellcheck disable=SC2155

# allow checks with $?, as cmds output are needed in variable
# shellcheck disable=SC2181
# ignore three times unused variables
# shellcheck disable=SC2034
# the echo is used for some crazy variable
# shellcheck disable=SC2116
# x-prefix is used x times in very old code. It looks like being used for checking for emptyness
# shellcheck disable=SC2268
# string-replacement is supported by busybox-ash
# shellcheck disable=SC3060
# exponential-calculations are supported by busybox-ash
# shellcheck disable=SC3019

. /lib/functions.sh
. /lib/functions/semver.sh
. /etc/openwrt_release
. /lib/functions/guard.sh

if [ -f /etc/freifunk_release ]; then
    . /etc/freifunk_release
    DISTRIB_ID="Freifunk Berlin"
    DISTRIB_RELEASE=$FREIFUNK_RELEASE
fi

# possible cases:
# 1) firstboot with kathleen --> uci system.version not defined
# 2) upgrade from kathleen --> uci system.version defined
# 3) upgrade from non kathleen / legacy --> no uci system.version

OLD_VERSION=$(uci -q get system.@system[0].version)
# remove "special-version" e.g. "-alpha+3a7d"; only work on "basic" semver-strings
VERSION=${DISTRIB_RELEASE%%-*}

log() {
    logger -s -t freifunk-berlin-migration "$@"
    echo >>/root/migrate.log "$@"
}

if [ "Freifunk Berlin" = "${DISTRIB_ID}" ]; then
    log "Migration is running on a Freifunk Berlin system"
else
    log "no Freifunk Berlin system detected ..."
    exit 0
fi

# when upgrading from a pre-kathleen installation, there sould be
# at least on "very old file" in /etc/config ...
#
FOUND_OLD_FILE=false
# create helper to compare file ctime (1. Sep. 2014)
touch -d "201409010000" /tmp/timestamp
for testfile in /etc/config/*; do
    if [ "${testfile}" -ot /tmp/timestamp ]; then
        FOUND_OLD_FILE=true
        echo "guessing pre-kathleen firmware as of ${testfile}"
    fi
done
rm -f /tmp/timestamp

if [ -n "${OLD_VERSION}" ]; then
    # case 2)
    log "normal migration within Release ..."
elif [ ${FOUND_OLD_FILE} = true ]; then
    # case 3)
    log "migrating from legacy Freifunk Berlin system ..."
    OLD_VERSION='0.0.0'
else
    # case 1)
    log "fresh install - no migration"
    # add system.version with the new version
    log "Setting new system version to ${VERSION}; no migration needed."
    uci set system.@system[0].version="${VERSION}"
    uci commit
    exit 0
fi

migrate_profiles() {
    # migrate to the latest /etc/config/profile_* and /etc/config/freifunk
    log "Updating Community-Profiles."
    rm /etc/config/profile_* # remove deprecated profiles
    cp /rom/etc/config/profile_* /etc/config

    log "Importing updates to /etc/config/freifunk"
    CONTACT=$(uci show freifunk.contact)
    COMMUNITY=$(uci show freifunk.community)
    cp /rom/etc/config/freifunk /etc/config
    OLDIFS=$IFS
    IFS=$(printf '\n')
    for i in $CONTACT $COMMUNITY; do
        key=$(echo "$i" | cut -d = -f 1)
        val=$(echo "$i" | cut -d = -f 2)
        val=$(echo "$val" | sed s/\'//g)
        if [ "$val" != "defaults" ]; then
            uci set "$key=${val}"
        fi
    done
    IFS=$OLDIFS
    uci commit freifunk
}

ensure_profiled() {
    # since the files in /etc/profile.d are carried over though upgrades,
    # make sure that the ones in /rom are the ones being used
    log "updating /etc/profile.d"
    cp /rom/etc/profile.d/* /etc/profile.d
}

update_openvpn_remote_config() {
    # use dns instead of ips for vpn servers (introduced with 0.1.0)
    log "Setting openvpn.ffvpn.remote to vpn03.berlin.freifunk.net"
    uci delete openvpn.ffvpn.remote
    uci add_list openvpn.ffvpn.remote='vpn03.berlin.freifunk.net 1194 udp'
    uci add_list openvpn.ffvpn.remote='vpn03-backup.berlin.freifunk.net 1194 udp'
}

update_dhcp_lease_config() {
    # set lease time to 5 minutes (introduced with 0.1.0)
    local current_leasetime="$(uci get dhcp.dhcp.leasetime)"
    if [ "x${current_leasetime}" = "x" ]; then
        log "Setting dhcp lease time to 5m"
        uci set dhcp.dhcp.leasetime='5m'
    fi
}

update_wireless_ht20_config() {
    # set htmode to ht20 (introduced with 0.1.0)
    log "Setting htmode to HT20 for radio0"
    uci set wireless.radio0.htmode='HT20'
    local radio1_present="$(uci get wireless.radio1.htmode)"
    # set htmode if radio1 is present
    if [ "x${radio1_present}" != x ]; then
        log "Setting htmode to HT20 for radio1."
        uci set wireless.radio1.htmode='HT20'
    fi
}

update_luci_statistics_config() {
    # if users disabled stats with the wizard some settings need be corrected
    # so they can enable stats later
    log "remove luci_statistics.rrdtool.enable"
    log "remove luci_statistics.collectd.enable"
    uci delete luci_statistics.rrdtool.enable
    uci delete luci_statistics.collectd.enable

    # enable luci_statistics service
    log "enable luci_statistics service"
    /etc/init.d/luci_statistics enable
}

update_collectd_memory_leak_hotfix() {
    # Remove old hotfixes for collectd RAM issues on 32MB routers
    # see https://github.com/freifunk-berlin/firmware/issues/217
    CRONTAB="/etc/crontabs/root"
    test -f $CRONTAB || touch $CRONTAB
    sed -i '/luci_statistics$/d' $CRONTAB
    sed -i '/luci_statistics restart$/d' $CRONTAB
    /etc/init.d/cron restart

    if [ "$(grep MemTotal: </proc/meminfo | awk \{'print $2'\})" -lt "65536" ]; then
        uci set luci_statistics.collectd_ping.enable=0
        uci set luci_statistics.collectd_rrdtool.enable=0
    fi
}

update_olsr_smart_gateway_threshold() {
    # set SmartGatewayThreshold if not set
    local threshold="$(uci get olsrd.@olsrd[0].SmartGatewayThreshold)"
    if [ "x${threshold}" = x ]; then
        log "Setting SmartGatewayThreshold to 50."
        uci set olsrd.@olsrd[0].SmartGatewayThreshold='50'
    fi
}

fix_olsrd_txtinfo_port() {
    uci set "$(uci show olsrd | grep olsrd_txtinfo | cut -d '=' -f 1 | sed 's/library/port/')"=2006
    uci set "$(uci show olsrd6 | grep olsrd_txtinfo | cut -d '=' -f 1 | sed 's/library/port/')"=2006
}

add_openvpn_mssfix() {
    uci set openvpn.ffvpn.mssfix=1300
}

openvpn_ffvpn_hotplug() {
    uci set openvpn.ffvpn.up="/lib/freifunk/ffvpn-up.sh"
    uci set openvpn.ffvpn.nobind=0
    /etc/init.d/openvpn disable
    for entry in $(uci show firewall | grep Reject-VPN-over-ff | cut -d '=' -f 1); do
        uci delete "${entry%.name}"
    done
    uci delete freifunk-watchdog
    crontab -l | grep -v "/usr/sbin/ffwatchd" | crontab -
}

update_collectd_ping() {
    uci set luci_statistics.collectd_ping.Interval=10
    uci set luci_statistics.collectd_ping.Hosts=ping.berlin.freifunk.net
}

fix_qos_interface() {
    for rule in $(uci show qos | grep qos.wan); do
        uci set "${rule/wan/ffvpn}"
    done
    uci delete qos.wan
}

sgw_rules_to_fw3() {
    uci set firewall.zone_freifunk.device=tnl_+
    sed -i '/iptables -I FORWARD -o tnl_+ -j ACCEPT$/d' /etc/firewall.user
}

remove_dhcp_interface_lan() {
    uci -q delete dhcp.lan
}

change_olsrd_dygw_ping() {
    change_olsrd_dygw_ping_handle_config() {
        local config=$1
        local library=''
        config_get library "$config" library
        if [ "$library" = 'olsrd_dyn_gw.so.0.5' ]; then
            uci delete "olsrd.$config.Ping"
            uci add_list "olsrd.$config.Ping='85.214.20.141'"   # dns.digitalcourage.de
            uci add_list "olsrd.$config.Ping='213.73.91.35'"    # dnscache.ccc.berlin.de
            uci add_list "olsrd.$config.Ping='194.150.168.168'" # dns.as250.net
            return 1
        fi
    }
    reset_cb
    config_load olsrd
    config_foreach change_olsrd_dygw_ping_handle_config LoadPlugin
}

fix_dhcp_start_limit() {
    # only set start and limit if we have a dhcp section
    if (uci -q show dhcp.dhcp); then
        # only alter start and limit if not set by the user
        if ! (uci -q get dhcp.dhcp.start || uci -q get dhcp.dhcp.limit); then
            local netmask
            local prefix
            # get network-length
            if netmask="$(uci -q get network.dhcp.netmask)"; then
                # use ipcalc.sh to and get prefix-length only
                prefix="$(ipcalc.sh 0.0.0.0 "${netmask}" | grep PREFIX | awk -F "=" '{print $2}')"
                # compute limit (2^(32-prefix)-3) with arithmetic evaluation
                limit=$((2 ** (32 - prefix) - 3))
                uci set dhcp.dhcp.start=2
                uci set dhcp.dhcp.limit=${limit}
                log "set new dhcp.limit and dhcp.start on interface dhcp"
            else
                log "interface dhcp has no netmask assigned. not fixing dhcp.limit"
            fi
        else
            log "interface dhcp has start and limit defined. not changing it"
        fi
    else
        log "interface dhcp has no dhcp-config at all"
    fi
}

delete_system_latlon() {
    log "removing obsolete uci-setting system.system.latlon"
    uci -q delete system.@system[0].latlon
}

update_berlin_owm_api() {
    if [ "$(uci get freifunk.community.name)" = "berlin" ]; then
        log "updating Berlin OWM API URL"
        uci set freifunk.community.owm_api="http://util.berlin.freifunk.net"
    fi
}

fix_olsrd6_watchdog_file() {
    log "fix olsrd6 watchdog file"
    uci set "$(uci show olsrd6 | grep "/var/run/olsrd.watchdog" | cut -d '=' -f 1)"=/var/run/olsrd6.watchdog
}

quieten_dnsmasq() {
    log "quieten dnsmasq"
    uci set dhcp.@dnsmasq[0].quietdhcp=1
}

vpn03_udp4() {
    log "set VPN03 to UDPv4 only"
    uci delete openvpn.ffvpn.remote
    uci add_list openvpn.ffvpn.remote='vpn03.berlin.freifunk.net 1194'
    uci add_list openvpn.ffvpn.remote='vpn03-backup.berlin.freifunk.net 1194'
    uci set openvpn.ffvpn.proto=udp4
}

set_ipversion_olsrd6() {
    uci set olsrd6.@olsrd[0].IpVersion=6
}

bump_repo() {
    # adjust the opkg packagefeed to point to new version
    local FEED_LINE=$(grep "openwrt_falter" /rom/etc/opkg/customfeeds.conf)
    log "adjusting packagefeed to new version feed"
    sed -i "s,src\/gz.openwrt_falter.https\?:\/\/firmware\.berlin\.freifunk\.net.*,$FEED_LINE,g" /etc/opkg/customfeeds.conf
}

r1_0_0_vpn03_splitconfig() {
    log "changing guard-entry for VPN03 from openvpn to vpn03-openvpn (config-split for VPN03)"
    guard_rename openvpn vpn03_openvpn # to guard the current settings of package "freifunk-berlin-vpn03-files"
}

r1_0_0_no_wan_restart() {
    crontab -l | grep -v "^0 6 \* \* \* ifup wan$" | crontab -
}

r1_0_0_firewallzone_uplink() {
    log "adding firewall-zone for VPN / Uplink"
    uci set firewall.zone_ffuplink=zone
    uci set firewall.zone_ffuplink.name=ffuplink
    uci set firewall.zone_ffuplink.input=REJECT
    uci set firewall.zone_ffuplink.forward=ACCEPT
    uci set firewall.zone_ffuplink.output=ACCEPT
    uci set firewall.zone_ffuplink.network=ffuplink
    # remove ffvpn from zone freifunk
    ffzone_new=$(uci get firewall.zone_freifunk.network | sed "s/ ffvpn//g")
    log " zone freifunk has now interfaces: ${ffzone_new}"
    uci set firewall.zone_freifunk.network="${ffzone_new}"
    log " setting up forwarding for ffuplink"
    uci set firewall.fwd_ff_ffuplink=forwarding
    uci set firewall.fwd_ff_ffuplink.src=freifunk
    uci set firewall.fwd_ff_ffuplink.dest=ffuplink
}

r1_0_0_change_to_ffuplink() {
    change_olsrd_dygw_ping_iface() {
        local config=$1
        local lib=''
        config_get lib "$config" library
        if [ -z "${lib##olsrd_dyn_gw.so*}" ]; then
            uci set "olsrd.$config.PingCmd"='ping -c 1 -q -I ffuplink %s'
            return 1
        fi
    }
    remove_routingpolicy() {
        local config=$1
        case "$config" in
            olsr_*_ffvpn_ipv4*)
                log "  network.$config"
                uci delete "network.$config"
                ;;
            *) ;;
        esac
    }

    log "changing interface ffvpn to ffuplink"
    log " setting wan as bridge"
    uci set network.wan.type=bridge
    log " renaming interface ffvpn"
    uci rename network.ffvpn=ffuplink
    uci set network.ffuplink.ifname=ffuplink
    log " updating VPN03-config"
    uci rename openvpn.ffvpn=ffuplink
    uci set openvpn.ffuplink.dev=ffuplink
    uci set openvpn.ffuplink.status="/var/log/openvpn-status-ffuplink.log"
    uci set openvpn.ffuplink.key="/etc/openvpn/ffuplink.key"
    uci set openvpn.ffuplink.cert="/etc/openvpn/ffuplink.crt"
    log " renaming VPN03 certificate files"
    mv /etc/openvpn/freifunk_client.crt /etc/openvpn/ffuplink.crt
    mv /etc/openvpn/freifunk_client.key /etc/openvpn/ffuplink.key
    log " updating statistics, qos, olsr to use ffuplink"
    # replace ffvpn by ffuplink
    ffuplink_new=$(uci get luci_statistics.collectd_interface.Interfaces | sed "s/ffvpn/ffuplink/g")
    uci set luci_statistics.collectd_interface.Interfaces="${ffuplink_new}"
    uci rename qos.ffvpn=ffuplink
    reset_cb
    config_load olsrd
    config_foreach change_olsrd_dygw_ping_iface LoadPlugin
    log " removing deprecated IP-rules"
    reset_cb
    config_load network
    config_foreach remove_routingpolicy rule
}

r1_0_0_update_preliminary_glinet_names() {
    case $(uci get system.led_wlan.sysfs) in
        "gl_ar150:wlan")
            log "correcting system.led_wlan.sysfs for GLinet AR150"
            uci set system.led_wlan.sysfs="gl-ar150:wlan"
            ;;
        "gl_ar300:wlan")
            log "correcting system.led_wlan.sysfs for GLinet AR300"
            uci set system.led_wlan.sysfs="gl-ar300:wlan"
            ;;
        "domino:blue:wlan")
            log "correcting system.led_wlan.sysfs for GLinet Domino"
            uci set system.led_wlan.sysfs="gl-domino:blue:wlan"
            ;;
    esac
}

r1_0_0_upstream() {
    log "applying upstream changes / sync with upstream"
    grep -q "^kernel.core_pattern=" /etc/sysctl.conf || echo >>/etc/sysctl.conf "kernel.core_pattern=/tmp/%e.%t.%p.%s.core"
    sed -i '/^net.ipv4.tcp_ecn=0/d' /etc/sysctl.conf
    grep -q "^128" /etc/iproute2/rt_tables || echo >>/etc/iproute2/rt_tables "128	prelocal"
    cp /rom/etc/inittab /etc/inittab
    cp /rom/etc/profile /etc/profile
    cp /rom/etc/hosts /etc/hosts
    log " checking for user dnsmasq"
    group_exists "dnsmasq" || group_add "dnsmasq" "453"
    user_exists "dnsmasq" || user_add "dnsmasq" "453" "453"
}

r1_0_0_set_uplinktype() {
    log "storing used uplink-type"
    log " migrating from Kathleen-release, assuming VPN03 as uplink-preset"
    echo "" | uci import ffberlin-uplink
    uci set ffberlin-uplink.preset=settings
    uci set ffberlin-uplink.preset.current="vpn03_openvpn"
}

r1_0_1_set_uplinktype() {
    uci >/dev/null -q get ffberlin-uplink.preset && return 0

    log "storing used uplink-type for Hedy"
    uci set ffberlin-uplink.preset=settings
    uci set ffberlin-uplink.preset.current="unknown"
    if [ "$(uci -q get network.ffuplink_dev.type)" = "veth" ]; then
        uci set ffberlin-uplink.preset.current="no-tunnel"
    else
        case "$(uci -q get openvpn.ffuplink.remote)" in
            \'vpn03.berlin.freifunk.net*)
                uci set ffberlin-uplink.preset.current="vpn03_openvpn"
                ;;
            \'tunnel-gw.berlin.freifunk.net*)
                uci set ffberlin-uplink.preset.current="tunnelberlin_openvpn"
                ;;
        esac
    fi
    log " type set to $(uci get ffberlin-uplink.preset.current)"
}

r1_1_0_change_olsrd_lib_num() {
    log "remove suffix from olsrd plugins"
    change_olsrd_lib_num_handle_config() {
        local config=$1
        local v6=$2
        local library=''
        local librarywo=''
        config_get library "$config" library
        librarywo="$(echo "${library%%.*}")"
        uci set "olsrd$v6.$config.library=$librarywo"
        log " changed olsrd$v6 $librarywo"
    }
    reset_cb
    config_load olsrd
    config_foreach change_olsrd_lib_num_handle_config LoadPlugin
    config_load olsrd6
    config_foreach change_olsrd_lib_num_handle_config LoadPlugin 6

}

r1_1_0_notunnel_ffuplink() {
    if [ "$(uci -q get ffberlin-uplink.preset.current)" = "no-tunnel" ]; then
        log "update the ffuplink_dev to have a static macaddr if not already set"
        local macaddr=$(uci -q get network.ffuplink_dev.macaddr)
        if [ $? -eq 1 ]; then
            # Create a static random macaddr for ffuplink device
            # start with fe for ffuplink devices
            # See the website https://www.itwissen.info/MAC-Adresse-MAC-address.html
            macaddr="fe"
            for byte in 2 3 4 5 6; do
                macaddr=$macaddr$(dd if=/dev/urandom bs=1 count=1 2>/dev/null | hexdump -e '1/1 ":%02x"')
            done
            uci set network.ffuplink_dev.macaddr="$macaddr"
        fi
    fi
}

r1_1_0_notunnel_ffuplink_ipXtable() {
    if [ "$(uci -q get ffberlin-uplink.preset.current)" = "no-tunnel" ]; then
        log "update the ffuplink no-tunnel settings to use options ip4table and ip6table"
        uci set network.ffuplink.ip4table="ffuplink"
        uci set network.ffuplink.ip6table="ffuplink"
    fi
}

r1_1_0_olsrd_dygw_ping() {
    olsrd_dygw_ping() {
        local config=$1
        local lib=''
        config_get lib "$config" library
        local libname=${lib%%.*}
        if [ "$lib" = "olsrd_dyn_gw" ]; then
            uci del_list "olsrd.$config.Ping='213.73.91.35'"  # dnscache.ccc.berlin.de
            uci add_list "olsrd.$config.Ping='80.67.169.40'"  # www.fdn.fr/actions/dns
            uci del_list "olsrd.$config.Ping='85.214.20.141'" # old digitalcourage
            uci add_list "olsrd.$config.Ping='46.182.19.48'"  # new digitalcourage
            return 1
        fi
    }
    reset_cb
    config_load olsrd
    config_foreach olsrd_dygw_ping LoadPlugin
}

r1_0_2_update_dns_entry() {
    log "updating DNS-servers for interface dhcp from profile"
    uci set network.dhcp.dns="$(uci get "profile_$(uci get freifunk.community.name).interface.dns")"
}

r1_0_2_add_olsrd_garbage_collection() {
    crontab -l | grep "rm -f /tmp/olsrd\*core"
    if [ $? = 1 ]; then
        log "adding garbage collection of core files from /tmp"
        echo "23 4 * * *	rm -f /tmp/olsrd*core" >>/etc/crontabs/root
        /etc/init.d/cron restart
    fi
}

r1_1_0_remove_olsrd_garbage_collection() {
    log "removing garbage collection of core files from /tmp"
    crontab -l | grep -v "rm -f /tmp/olsrd\*core" | crontab -
    /etc/init.d/cron restart
}

r1_1_0_update_dns_entry() {
    network_interface_delete_dns() {
        local config=${1}
        uci -q del "network.${config}.dns"
    }
    reset_cb
    config_load network
    config_foreach network_interface_delete_dns interface
    uci set network.loopback.dns="$(uci get "profile_$(uci get freifunk.community.name).interface.dns")"
}

r1_1_0_update_uplink_notunnel_name() {
    log "update name of uplink-preset notunnel"
    local result=$(uci -q get ffberlin-uplink.preset.current)
    [ $? -eq 0 ] && [ "$result" = "no-tunnel" ] && uci set ffberlin-uplink.preset.current=notunnel
    result=$(uci -q get ffberlin-uplink.preset.previous)
    [ $? -eq 0 ] && [ "$result" = "no-tunnel" ] && uci set ffberlin-uplink.preset.previous=notunnel
    log "update name of uplink-preset notunnel done"
}

r1_1_0_firewall_remove_advanced() {
    firewall_remove_advanced() {
        uci -q delete "firewall.$1"
    }
    config_load firewall
    config_foreach firewall_remove_advanced advanced
}

r1_1_0_statistics_server() {
    log "Setting the statistcs server to \"monitor.berlin.freifunk.net\"."
    local result=$(uci -q get luci_statistics.\@collectd_network_server\[0\].host)
    [ $? -eq 0 ] && [ "$result" = "77.87.48.12" ] \
        && uci set luci_statistics.\@collectd_network_server\[0\].host=monitor.berlin.freifunk.net
}

r1_1_0_openwrt_19_07_updates() {
    log "performing updates to openwrt 19.07."
    uci set dhcp.@dnsmasq[0].nonwildcard="1"
    uci set dhcp.odhcpd.loglevel="4"
    uci set luci.main.ubuspath="/ubus/"
    uci set luci.apply="internal"
    uci set luci.apply.rollback="90"
    uci set luci.apply.holdoff="4"
    uci set luci.apply.timeout="5"
    uci set luci.apply.display="1.5"
    uci set luci.diag.dns="openwrt.org"
    uci set luci.diag.ping="openwrt.org"
    uci set luci.diag.route="openwrt.org"
    rpcd=i$(uci add rpcd rpcd)
    uci set rpcd.@rpcd[-1].socket="/var/run/ubus.sock"
    uci set rpcd.@rpcd[-1].timeout="30"
    uci add_list uhttpd.main.lua_prefix="/cgi-bin/luci=/usr/lib/lua/luci/sgi/uhttpd.lua"
    uci set defaults.default.system="1"
    uci del system.ntp.server
    uci add_list system.ntp.server="0.openwrt.pool.ntp.org"
    uci add_list system.ntp.server="1.openwrt.pool.ntp.org"
    uci add_list system.ntp.server="2.openwrt.pool.ntp.org"
    uci add_list system.ntp.server="3.openwrt.pool.ntp.org"
    uci set system.ntp.use_dhcp="0"
    uci set uhttpd.defaults.key_type="rsa"
    uci set uhttpd.defaults.ec_curve="P-256"
    uci set uhttpd.defaults.commonname="Freifunk-Falter"
    uci commit uhttpd
    /etc/init.d/uhttpd restart

    handle_ucitrack_system() {
        local config=${1}
        uci set "ucitrack.${config}.exec=/etc/init.d/log reload"
        uci add_list "ucitrack.${config}.affects=dhcp"
    }
    reset_cb
    config_load ucitrack
    config_foreach handle_ucitrack_system system

    handle_ucitrack_fstab() {
        local config=${1}
        uci delete "ucitrack.${config}.init"
        uci set "ucitrack.${config}.exec=/sbin/block mount"
    }
    reset_cb
    config_load ucitrack
    config_foreach handle_ucitrack_fstab fstab
}

r1_1_0_wifi_iface_names() {
    local count=0
    wifi_set_name() {
        local config=${1}
        # skip if there is already a name for this section
        [ "$(echo "${config%%[0-9]*}")" != "cfg" ] && return

        # determine a name for this section
        local ifname=$(uci -q get "wireless.${config}.ifname")
        [ "X${ifname}X" = "XX" ] && ifname="wifinet${count}"
        ifname=$(echo "$ifname" | sed 's/-/_/g')
        uci -q rename "wireless.$config=$ifname"
        count=$((count + 1))
    }
    config_load wireless
    config_foreach wifi_set_name wifi-iface
}

r1_1_0_ffwizard() {
    log "adding new fields to ffwizard"
    uci set ffwizard.upgrade="upgrade"

    # add the new meshmode_$device field to settings
    handle_wifi_iface() {
        local config=$1
        local mode=''
        local device=''
        config_get mode "$config" mode
        config_get device "$config" device
        [ "$mode" = "mesh" ] && mode="80211s"
        [ "$mode" != "adhoc" ] && [ "$mode" != "80211s" ] && return

        uci set "ffwizard.settings.meshmode_${device}=${mode}"
    }

    reset_cb
    config_load wireless
    config_foreach handle_wifi_iface wifi-iface
}

r1_1_0_statistics() {
    log "adding new options to luci_statistics"
    uci -q set luci_statistics.collectd_memory.ValuesAbsolute=1
}

r1_1_0_sharenet_setup() {
    local sharenet=$(uci get ffwizard.settings.sharenet)
    if [ 1 -ne "$sharenet" ]; then
        log "disabling ffuplink because sharenet is not set to 1"
        uci set network.ffuplink.disabled=1
    else
        log "enabling ffuplink because sharenet is set to 1"
        uci set network.ffuplink.disabled=0
    fi
}

r1_1_1_rssiled() {
    # make sure that the rssileds are set properly to the mesh interface
    log "setting rssidleds to the wireless mesh interface"
    handle_wifi_iface_rssiled() {
        local config=$1
        local mode=''
        local ifname=''
        config_get mode "$config" mode
        config_get ifname "$config" ifname
        [ "$mode" != "adhoc" ] && [ "$mode" != "mesh" ] && return

        local rssidev=${ifname%%-*}
        local result=$(uci -q get "system.rssid_${rssidev}.dev")
        if [ "X${result}X" != "XX" ]; then
            # we need to stop the rssileds service before making the new setting
            /etc/init.d/rssileds stop
            uci set "system.rssid_${rssidev}.dev=${ifname}"
            # we need to commit the system changes early if we want the
            # rssileds service to start correctly
            uci commit system
            /etc/init.d/rssileds start
        fi
    }

    reset_cb
    config_load wireless
    config_foreach handle_wifi_iface_rssiled wifi-iface
}

r1_1_2_dnsmasq_ignore_wan() {
    # introduced at 1157a128140962364b5392c7857c174c1d34409f
    local notinterfaces=$(uci -q get dhcp.@dnsmasq[0].notinterface)
    local found=0
    for interface in ${notinterfaces}; do
        if [ "X${interface}X" = "XwanX" ]; then
            found=1
        fi
    done

    if [ "X${found}X" = "X0X" ]; then
        log "adjust dnsmasq to ignore wan iface in log"
        uci add_list dhcp.@dnsmasq[0].notinterface='wan'
        uci commit dhcp
    fi
}

r1_1_2_peerdns_ffuplink() {
    # migrates for a4a0693cc1ebb536350e9228d54c2291f4df8133
    log "set peerdns=0 on ffuplink"
    uci set network.ffuplink.peerdns=0
    uci commit network
}

r1_1_2_new_sysctl_conf() {
    # the sysctl changed considerably since hedy. There's not much inportant to preserve,
    # just overwrite with new default. Effectively this would clear the file.
    log "migrating /etc/sysctl.conf"
    cp -f /rom/etc/sysctl.conf /etc/sysctl.conf
}

r1_1_2_rssiled() {
    # rerund the r1_1_1_rssileds function because of a previous code error
    r1_1_1_rssiled
}

r1_2_0_dhcp() {
    uci -q get dhcp.@dnsmasq[0].ednspacket_max
    if [ $? -eq 1 ]; then
        log "adding ednspacket_max=1232 to dnsmasq config"
        uci set dhcp.@dnsmasq[0].ednspacket_max=1232
        uci commit dhcp
    fi
}

r1_2_0_fw_zones() {
    handle_zone() {
        local cfg=$1
        local name=''
        local nets=''
        local devs=''

        config_get name "$cfg" name
        config_get nets "$cfg" network
        config_get devs "$cfg" device

        log "ensuring that the networks and devices in zone ${name} are stored as a list"
        uci -q delete "firewall.$cfg.network"
        for net in $nets; do
            uci add_list "firewall.$cfg.network=$net"
        done
        uci -q delete "firewall.$cfg.device"
        for dev in $devs; do
            uci add_list "firewall.$cfg.device=$dev"
        done
    }

    reset_cb
    config_load firewall
    config_foreach handle_zone zone

    uci commit firewall
}

r1_2_0_fw_synflood() {
    log "changing firewall default from syn_flood to synflood_protect"
    uci -q delete firewall.@defaults[0].syn_flood
    uci -q set firewall.@defaults[0].synflood_protect=1
    uci commit firewall
}

r1_2_0_statistics() {
    local enabled=$(uci -q get luci_statistics.collectd_interface.enable)
    log "adding dhcpleases section to luci_statistics"
    uci set luci_statistics.collectd_dhcpleases=statistics
    uci set luci_statistics.collectd_dhcpleases.enable="$enabled"
    uci set luci_statistics.collectd_dhcpleases.Path='/tmp/dhcp.leases'
    uci commit luci_statistics
}

r1_2_0_network() {
    log "adjusting network config to openwrt 21.02 syntax"

    # Make sure wan is a bridge.
    WANDEV=$(uci -q get network.wan.ifname)
    echo "${WANDEV}" | grep ^br- >/dev/null
    BRIDGECHECK=$?

    if [ "X${WANDEV}X" = "XX" ]; then
        # This device does not have a wan port. Create a wan device without
        # a physical port.  This makes it easier to change a single
        # port device from the client network to wan. This is also needed
        # in the case where the user decides to use the "notunnel" variant
        log "creating a wan bridge device and wan/wan6 interfaces"
        NEWDEV=$(uci -q add network device)
        uci -q set "network.$NEWDEV.type=bridge"
        uci -q set "network.$NEWDEV.name=br-wan"

        # create a wan interface, even if it can't do anything
        uci -q set network.wan=interface
        uci -q set network.wan.device="br-wan"
        uci -q set network.wan.proto="dhcp"

        # create a wan6 interface, even if it can't do anything
        uci -q set network.wan6=interface
        uci -q set network.wan6.device="br-wan"
        uci -q set network.wan6.proto="dhcp6"

    elif [ $BRIDGECHECK = "0" ]; then
        # The wan device is a bridge (ex DSA with multiple physical ports)
        # everything should be set up fine in this case
        : # do nothing
    else
        # The wan device is not a bridge.  Change it to a bridge
        log "changing wan device to a bridge"
        NEWDEV=$(uci -q add network device)
        uci -q set "network.$NEWDEV.type=bridge"
        uci -q set "network.$NEWDEV.name=br-wan"
        for port in $WANDEV; do
            uci -q add_list "network.$NEWDEV.ports=$port"
        done

        uci -q set network.wan.device="br-wan"
        uci -q delete network.wan.type
        uci -q delete network.wan.ifname

        uci -q set network.wan6.device="br-wan"
        uci -q delete network.wan6.type
        uci -q delete network.wan6.ifname
    fi

    # change ffuplink from ifname to device (may or may not be a
    # tunneldigger interface).
    dev=$(uci -q get network.ffuplink.ifname)
    if [ $? -eq 0 ]; then
        log "changing ffuplink interface from ifname syntax to device"
        uci -q delete network.ffuplink.ifname
        uci -q set network.ffuplink.device="$dev"
        uci -q delete network.ffuplink.type
    fi

    # change dhcp from ifname to device
    ports=$(uci -q get network.dhcp.ifname)
    if [ $? -eq 0 ]; then
        log "changing dhcp interface from ifname syntax to device"
        NEWDEV=$(uci -q add network device)
        uci -q set "network.$NEWDEV.type=bridge"
        uci -q set "network.$NEWDEV.name=br-dhcp"
        for port in $ports; do
            uci -q add_list "network.$NEWDEV.ports=$port"
        done
        uci -q delete network.dhcp.ifname
        uci -q set network.dhcp.device="br-dhcp"
        uci -q delete network.dhcp.type
    fi

    # change loopback from ifname to device
    dev=$(uci -q get network.loopback.ifname)
    if [ $? -eq 0 ]; then
        log "changing loopback interface from ifname sytax to device"
        uci -q delete network.loopback.ifname
        uci -q set "network.loopback.device=$dev"
    fi

    # change tunl0 from ifname to device
    dev=$(uci -q get network.tunl0.ifname)
    if [ $? -eq 0 ]; then
        log "changing tunl0 interface from ifname sytax to device"
        uci -q delete network.tunl0.ifname
        uci -q set "network.tunl0.device=$dev"
    fi

    # ensure all interfaces created by tunneldigger are changed from
    # ifname to device. This should handle bbbdigger, pdmdigger and
    # any other community mesh tunneldigger interfaces.  In the case
    # of ffuplink, it will fail through the second if clause as it has
    # already been taken care of explicitly above (ffuplink can also
    # be a notunnel interface).
    handle_tunneldigger_interface() {
        local config=$1
        local interface=''
        config_get interface "$config" interface "unset"
        if [ "${interface}" != "unset" ]; then
            local dev=$(uci -q get "network.${interface}.ifname")
            if [ "X${dev}X" != "XX" ]; then
                log "changing ${interface} interface from ifname syntax to device"
                uci -q delete "network.${interface}.ifname"
                uci -q set "network.${interface}.device=$dev"
                uci -q delete "network.${interface}.type"
            fi
        fi
    }
    reset_cb
    config_load tunneldigger
    config_foreach handle_tunneldigger_interface broker

    uci commit network
}

r1_2_0_olsrd() {
    log "adding new procd section to olsrd and olsrd6"
    uci set olsrd.procd=procd
    uci set olsrd.procd.respawn_threshold=3600
    uci set olsrd.procd.respawn_timeout=15
    uci set olsrd.procd.respawn_retry=0

    uci set olsrd6.procd=procd
    uci set olsrd6.procd.respawn_threshold=3600
    uci set olsrd6.procd.respawn_timeout=15
    uci set olsrd6.procd.respawn_retry=0

    log "removing olsrd_watchdog settings in olsrd and olsrd6"
    handle_watchdog_plugin() {
        local config=$1
        local confname=$2
        local library=""
        config_get library "$config" library
        if [ "$library" = "olsrd_watchdog" ]; then
            uci -q delete "$confname.$config"
        fi
    }

    reset_cb
    config_load olsrd
    config_foreach handle_watchdog_plugin LoadPlugin olsrd

    reset_cb
    config_load olsrd6
    config_foreach handle_watchdog_plugin LoadPlugin olsrd6

    uci commit olsrd
    uci commit olsrd6
}

r1_2_0_owm() {
    log "removing old owm.lua cronjob in exchange with the new owm.sh script"
    crontab -l | grep -v "owm.lua" | crontab -
    /etc/init.d/cron restart
}

r1_2_0_dynbanner() {
    log "removing old dynbanner.sh as now it is called 10_dynbanner.sh"
    rm -f /etc/profile.d/dynbanner.sh
}

r1_2_0_fixbbbdigger() {
    handle_device() {
        local config=$1
        local name=$(uci get "network.$config.name")
        if [ "$name" = "bbbdiggger" ]; then
            log "fixing misspelling of bbbdigger device section"
            uci set "network.$config.name=bbbdigger"
            uci commit network
        fi
    }
    reset_cb
    config_load network
    config_foreach handle_device device
}

r1_2_0_tunneldigger_srv() {
    local section=$1
    local srv=$2
    local result=$(uci -q get "tunneldigger.${section}")
    # check to make sure such a section is defined}
    if [ "$result" = "broker" ]; then
        log "updating server list for $section"
        uci -q delete "tunneldigger.${section}.address"
        uci -q set "tunneldigger.${section}.srv=$srv"
        uci commit tunneldigger
        /etc/init.d/tunneldigger restart "$section"
    fi
}

r1_2_0_ucitrack() {
    # in case there are multiple freifunk-policytrouing sections, remove extras
    log "removing extra freifunk-policyrouting sections from ucitrack"
    while [ "$(uci -q get ucitrack.@freifunk-policyrouting[1])" = "freifunk-policyrouting" ]; do
        uci -q delete ucitrack.@freifunk-policyrouting[1]
    done

    # remove radvd from the list of network affects
    log "removing radvd from ucitrack network settings"
    uci -q del_list ucitrack.@network[0].affects=radvd

    uci commit ucitrack
}

r1_2_1_dynbanner() {
    log "removing old dynbanner.sh as now it is called 10_dynbanner.sh"
    rm -f /etc/profile.d/dynbanner.sh
}

r1_2_1_rpcd() {
    if [ "$(uci get rpcd.@rpcd[0].socket)" != "/var/run/ubus/ubus.sock" ]; then
        log "ensuring that the ubus sock is set correctly"
        uci set rpcd.@rpcd[0].socket='/var/run/ubus/ubus.sock'
        uci commit rpcd
    fi
}

r1_2_1_ffwizard() {
    log "removing unneeded mode_radioX options from ffwizard"
    handle_wifi_device() {
        local section=$1
        uci -q del "ffwizard.settings.mode_${section}"
    }
    reset_cb
    config_load wireless
    config_foreach handle_wifi_device wifi-device

    uci commit ffwizard
}

r1_2_1_olsrd_watchdog_crontab() {
    log "removing the olsrds watchdog script from crontab"
    crontab -l | grep -v "ff_olsr_watchdog" | crontab -
    /etc/init.d/cron restart
}

r1_2_2_https_interface() {
    log "enabling redirection to TLS-encrypted LuCI-Interface"
    uci set uhttpd.main.redirect_https=1
    uci commit uhttpd
    service uhttpd reload
}

r1_2_3_update_dns() {
    log "updating dns servers in network-config"
    uci set network.loopback.dns='46.182.19.48 80.67.169.40 194.150.168.168 9.9.9.10 149.112.112.10 2001:910:800::12 2a02:2970:1002::18 2620:fe::10 2620:fe::fe:10'
    uci commit network
    log "updating dns-servers in freifunk-file"
    uci set freifunk.interface.dns='8.8.8.8 141.1.1.1'
    uci commit freifunk
    service network restart
}

r1_2_3_update_owm_cron() {
    log "make OWM cron run twice per hour"
    test -f /etc/crontabs/root || touch /etc/crontabs/root
    OWM="/usr/sbin/owm.sh"
    SEED="$(dd if=/dev/urandom bs=2 count=1 2>&- | hexdump | if read -r line; then echo "0x${line#* }"; fi)"
    MIN1="$((SEED % 29))"
    MIN2="$((MIN1 + 30))"
    (
        crontab -l | grep -v "$OWM"
        echo "$MIN1,$MIN2 * * * * test -e $OWM && $OWM"
    ) | crontab -
    /etc/init.d/cron restart
}

r1_3_0_update_dns() {
    reset_cb

    log "updating dns servers in olsrd dyngw config"
    r1_3_0_update_dns_olsrd_dyngw() {
        local config=$1
        local library=''
        config_get library "$config" library
        if [[ "$library" =~ olsrd_dyn_gw ]]; then
            uci delete "olsrd.$config.Ping"
            uci add_list "olsrd.$config.Ping=194.150.168.168"
            uci add_list "olsrd.$config.Ping=9.9.9.10"
            uci add_list "olsrd.$config.Ping=149.112.112.10"
        fi
    }
    config_load olsrd
    config_foreach r1_3_0_update_dns_olsrd_dyngw LoadPlugin
    uci commit olsrd

    log "updating dns servers in network config"
    uci set network.loopback.dns='194.150.168.168 9.9.9.10 149.112.112.10'
    uci commit network

    log "updating dns servers in freifunk config"
    uci set freifunk.interface.dns='194.150.168.168 9.9.9.10 149.112.112.10'
    uci commit freifunk

    reload_config
}

r1_3_0_autoupdate_url() {
    uci set autoupdate.cfg.url=https://firmware.berlin.freifunk.net/stable/autoupdate.json
    uci delete autoupdate.cfg.fw_server_fqdn
    uci delete autoupdate.cfg.selector_fqdn
    uci commit autoupdate
}

r1_3_0_remove_unused_stuff() {
    log "remove unused stuff"
    rm -f /etc/config/openvpn
    rm -f /etc/openvpn/ffuplink.crt
    rm -f /etc/openvpn/ffuplink.key
    rm -f /etc/luci-uploads/cbid.ffuplink.1.cert
    rm -f /etc/luci-uploads/cbid.ffuplink.1.key
    guard_delete tunnelberlin_openvpn
    guard_delete vpn03_openvpn
    [ -f /etc/init.d/olsrd2 ] && /etc/init.d/olsrd2 disable || true
    rm -f /etc/config/olsrd2
    guard_delete olsrd2
    [ -f /etc/init.d/olsrd6 ] && /etc/init.d/olsrd6 disable || true
    rm -f /etc/config/olsrd6
    guard_delete olsrd6
}

migrate() {
    log "Migrating from ${OLD_VERSION} to ${VERSION}."

    # always use the most recent profiles and update repo-link
    migrate_profiles
    bump_repo
    ensure_profiled

    if semverLT "${OLD_VERSION}" "0.1.0"; then
        update_openvpn_remote_config
        update_dhcp_lease_config
        update_wireless_ht20_config
        update_luci_statistics_config
        update_olsr_smart_gateway_threshold
    fi

    if semverLT "${OLD_VERSION}" "0.1.1"; then
        update_collectd_memory_leak_hotfix
        fix_olsrd_txtinfo_port
    fi

    if semverLT "${OLD_VERSION}" "0.1.2"; then
        add_openvpn_mssfix
    fi

    if semverLT "${OLD_VERSION}" "0.2.0"; then
        update_berlin_owm_api
        update_collectd_ping
        fix_qos_interface
        remove_dhcp_interface_lan
        openvpn_ffvpn_hotplug
        sgw_rules_to_fw3
        change_olsrd_dygw_ping
        fix_dhcp_start_limit
        delete_system_latlon
        fix_olsrd6_watchdog_file
    fi

    if semverLT "${OLD_VERSION}" "0.3.0"; then
        quieten_dnsmasq
    fi

    if semverLT "${OLD_VERSION}" "1.0.0"; then
        vpn03_udp4
        set_ipversion_olsrd6
        r1_0_0_vpn03_splitconfig
        r1_0_0_no_wan_restart
        r1_0_0_firewallzone_uplink
        r1_0_0_change_to_ffuplink
        r1_0_0_update_preliminary_glinet_names
        r1_0_0_upstream
        r1_0_0_set_uplinktype
    fi

    if semverLT "${OLD_VERSION}" "1.0.1"; then
        r1_0_1_set_uplinktype
    fi

    if semverLT "${OLD_VERSION}" "1.0.2"; then
        r1_1_0_notunnel_ffuplink_ipXtable
        r1_1_0_notunnel_ffuplink
        r1_0_2_update_dns_entry
        r1_0_2_add_olsrd_garbage_collection
        guard "ffberlin_uplink"
    fi

    if semverLT "${OLD_VERSION}" "1.1.0"; then
        r1_1_0_change_olsrd_lib_num
        r1_1_0_olsrd_dygw_ping
        r1_1_0_update_dns_entry
        r1_1_0_update_uplink_notunnel_name
        r1_1_0_remove_olsrd_garbage_collection
        r1_1_0_firewall_remove_advanced
        r1_1_0_statistics_server
        r1_1_0_openwrt_19_07_updates
        r1_1_0_wifi_iface_names
        r1_1_0_ffwizard
        r1_1_0_statistics
        r1_1_0_nosharenet_setup
    fi

    if semverLT "${OLD_VERSION}" "1.1.1"; then
        r1_1_1_rssiled
    fi

    if semverLT "${OLD_VERSION}" "1.1.2"; then
        r1_1_2_dnsmasq_ignore_wan
        r1_1_2_peerdns_ffuplink
        r1_1_2_new_sysctl_conf
        r1_1_2_rssiled
    fi

    if semverLT "${OLD_VERSION}" "1.2.0"; then
        r1_2_0_dhcp
        r1_2_0_fw_zones
        r1_2_0_fw_synflood
        r1_2_0_statistics
        r1_2_0_network
        r1_2_0_olsrd
        r1_2_0_owm
        r1_2_0_dynbanner
        r1_2_0_fixbbbdigger
        r1_2_0_tunneldigger_srv "ffuplink" "_tunnel._udp.berlin.freifunk.net"
        r1_2_0_tunneldigger_srv "bbbdigger" "_bbb-vpn._udp.berlin.freifunk.net"
        r1_2_0_ucitrack
    fi

    if semverLT "${OLD_VERSION}" "1.2.1"; then
        r1_2_1_dynbanner
        r1_2_1_rpcd
        r1_2_1_ffwizard
        r1_2_1_olsrd_watchdog_crontab
    fi

    if semverLT "${OLD_VERSION}" "1.2.2"; then
        r1_2_2_https_interface
    fi

    if semverLT "${OLD_VERSION}" "1.2.3"; then
        # run rssid-migration again, as there was a typo in an variable
        r1_1_1_rssiled
        # fix https://github.com/freifunk-berlin/falter-packages/issues/350
        r1_2_0_ucitrack
        r1_2_1_dynbanner
        r1_2_2_https_interface
        r1_2_3_update_dns
        r1_2_3_update_owm_cron
    fi

    if semverLT "${OLD_VERSION}" "1.3.0"; then
        r1_3_0_autoupdate_url
        r1_3_0_update_dns
        r1_3_0_remove_unused_stuff
    fi

    # overwrite version with the new version
    log "Setting new system version to ${VERSION}."
    uci set "system.@system[0].version=${VERSION}"

    uci commit

    log "Migration done."
}

migrate
