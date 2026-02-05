#!/bin/sh

# Upon restart of OLSRd, an UBUS add event is sent.  Here ubus is listened to
# and upon an add of OLSRd, all existing wireguard interfaces are added
# to OLSRd's interfaces via UBUS.

# shellcheck source=/dev/null
. $IPKG_INSTROOT/lib/functions.sh
# shellcheck source=/dev/null
. $IPKG_INSTROOT/usr/share/libubox/jshn.sh

logger -t wghelper "Starting wghelper"
ubus listen ubus.object.add | while IFS= read -r line; do
    json_load "$line"
    json_select ubus.object.add
    path=""
    json_get_var path path
    ifaces=""
    if [ "$path" = "olsrd" ]; then
        for iface in $(wg show interfaces); do
            ifaces=$ifaces" $iface"
            ubus call olsrd add_interface '{"ifname":'\""$iface"\"'}'
        done
        logger -t wghelper "Readded$ifaces after olsrd restart"
    fi
done
