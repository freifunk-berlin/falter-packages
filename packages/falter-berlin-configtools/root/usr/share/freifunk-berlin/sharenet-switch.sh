#!/bin/sh
#
# Copyright (C) 2018 holger@freifunk-berlin
# taken from https://github.com/openwrt-mirror/openwrt/blob/95f36ebcd774a8e93ad2a1331f45d1a9da4fe8ff/target/linux/ar71xx/base-files/etc/uci-defaults/02_network#L83
#
# This script should set a wan-Port if u want to share ur internet-connection.
# It should run after wizard did the necessary settings (sharenet = yes)
# what shall I do?
AUTOCOMMIT="no"

while getopts "c" option; do
        case "$option" in
                c)
                        AUTOCOMMIT="yes"
                        ;;
                *)
                        echo "Invalid argument '-$OPTARG'."
                        exit 1
                        ;;    
        esac              
done        
shift $((OPTIND - 1))

echo "usage $0 -c [commit]"

# should this script run?
if [ "$(uci get ffwizard.settings.sharenet 2> /dev/null)" == "0" ]; then
    echo 'dont share my internet' && exit 0
else
	if [ "$(uci get ffwizard.settings.sharenet 2> /dev/null)" == "1" ]; then
    		echo 'share my internet'
	else
		echo 'sharenet value unknown' && exit 0
	fi
fi
# can we change a network file?
[ -e /etc/config/network ] || echo 'file network not found' && exit 0
#
#touch /etc/config/network
#
. /lib/functions/uci-defaults.sh 	# routines that set switch etc
. /lib/ar71xx.sh 			# detect board name

# which board are we running on, what will we change?
# board=$(ar71xx_board_name) #is not sufficient for all boards
board=$(ubus -v call system board | jsonfilter -e '$.model')

echo $board

case "$board" in
#all0315n |\
#all0258n |\
#ja76pf2|\
#rocket-m-ti |\
#ubnt-unifi-outdoor)
#	ucidef_set_interface_lan "eth0 eth1"
#	;;
#
#ap132 |\
#wlr8100)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0t 2 3 4 5"
#	ucidef_add_switch_vlan "switch0" "2" "0t 1"
#	;;
#
#esr1750 |\
#epg5000)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0t 1 2 3 4"
#	ucidef_add_switch_vlan "switch0" "2" "0t 5"
#	;;
#
#ap136-010)
#	ucidef_set_interfaces_lan_wan "eth0" "eth1"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0 1 2 3 4"
#	ucidef_add_switch_vlan "switch0" "2" "5 6"
#	;;
#
#ap147-010)
#	ucidef_set_interfaces_lan_wan "eth0" "eth1"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0 1 2 3 4"
#	ucidef_add_switch_vlan "switch0" "2" "5 6"
#	;;
#
#ap136-020 |\
#ap135-020 |\
#tl-wr1043nd-v2 |\
#wzr-450hp2)
#	ucidef_set_interfaces_lan_wan "eth1" "eth0"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0 1 2 3 4"
#	ucidef_add_switch_vlan "switch0" "2" "5 6"
#	;;
#
#archer-c5 |\
#archer-c7 |\
#tl-wdr4900-v2)
#	ucidef_set_interfaces_lan_wan "eth1" "eth0"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0 2 3 4 5"
#	ucidef_add_switch_vlan "switch0" "2" "1 6"
#	;;
#
#bsb)
#	ucidef_set_interfaces_lan_wan "eth1" "eth0"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0 1 3"
#	ucidef_set_interface_wlan
#	;;
#
'GL AR150')
	echo $board found
	;;
'TP-Link CPE210 v1.1')
	echo $board found
#	should this be more sophisticated?
	uci set network.@switch_vlan[0].ports='0t 4'
	uci set network.@switch_vlan[1].ports='0t 5'
	;;
'Ubiquiti NanoStation M2' |\
'Ubiquiti NanoStation loco M2')
	echo $board found
#	eth tauschen?
	;;


#cpe510)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0t 5"
#	ucidef_add_switch_vlan "switch0" "2" "0t 4"
#	;;
#
#airgatewaypro)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0t 4"
#	ucidef_add_switch_vlan "switch0" "2" "0t 5"
#	;;
#
#db120 |\
#rb-2011l | \
#rb-2011uas |\
#rb-2011uias |\
#rb-2011uas-2hnd|\
#rb-2011uias-2hnd)
#	ucidef_set_interfaces_lan_wan "eth0.1 eth1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0t 2 3 4 5"
#	ucidef_add_switch_vlan "switch0" "2" "0t 1"
#	ucidef_add_switch "switch1" "1" "1"
#	ucidef_add_switch_vlan "switch1" "1" "0 1 2 3 4 5"
#
#	case "$board" in
#	rb-2011uas* | rb-2011uias | rb-2011uias-2hnd)
#		ucidef_set_interface_raw "sfp" "eth0.3"
#		ucidef_add_switch_vlan "switch0" "3" "0t 6"
#		;;
#	esac
#	;;
#
#dir-825-b1|\
#tew-673gru)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth1"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0 1 2 3 5t"
#	;;
#
#f9k1115v2)
#	ucidef_set_interfaces_lan_wan "eth1" "eth0"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "2 3 4 5 6"
#	ucidef_add_switch_vlan "switch0" "2" "0 1"
#	;;
#
#nbg460n_550n_550nh)
#	ucidef_set_interfaces_lan_wan "eth0" "eth1"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "0" "0 1 2 3 5"
#	;;
#
#nbg6716)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth1"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0t 1 2 3 4"
#	ucidef_add_switch_vlan "switch0" "2" "5 6"
#	;;
#
#ap143 |\
#rb-433 |\
#rb-433u)
#	ucidef_set_interfaces_lan_wan "eth1" "eth0"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "1 2 5"
#	;;
#
#rb-435g)
#	ucidef_set_interfaces_lan_wan "eth1" "eth0"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0 1 2"
#	;;
#
#rb-450)
#	ucidef_set_interfaces_lan_wan "eth1" "eth0"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0 1 2 3 5"
#	;;
#
#rb-450g |\
#routerstation-pro)
#	ucidef_set_interfaces_lan_wan "eth1" "eth0"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0 1 2 3 4"
#	;;
#
#ap136 |\
#rb-750gl |\
#rb-751g |\
#rb-951g-2hnd |\
#wzr-hp-g450h)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0t 2 3 4 5"
#	ucidef_add_switch_vlan "switch0" "2" "0t 1"
#	;;
#
#rb-951ui-2hnd)
#	ucidef_set_interfaces_lan_wan "eth1" "eth0"
#	;;
#
#rb-493g)
#	ucidef_set_interfaces_lan_wan "eth0 eth1.1" "eth1.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0 1 2 3 4"
#	ucidef_add_switch "switch1" "1" "1"
#	ucidef_add_switch_vlan "switch1" "1" "0t 1 2 3 4"
#	ucidef_add_switch_vlan "switch1" "2" "0t 5"
#	;;
#
#wzr-hp-g300nh2 |\
#pb92 |\
#ap113)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0t 1 3 4 5"
#	ucidef_add_switch_vlan "switch0" "2" "0t 2"
#	;;
#
#el-m150)
#	ucidef_set_interfaces_lan_wan "eth1" "eth0"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0 1 3"
#	;;
#
#tl-wdr4300|\
#tl-wr1041n-v2)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0t 2 3 4 5"
#	ucidef_add_switch_vlan "switch0" "2" "0t 1"
#	;;
#
#tl-wr1043nd)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "1 2 3 4 5t"
#	ucidef_add_switch_vlan "switch0" "2" "0 5t"
#	;;
#
#tl-wr2543n)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "1 2 3 4 9t"
#	ucidef_add_switch_vlan "switch0" "2" "0 9t"
#	;;
#
#tl-wr841n-v1|\
#tl-wr941nd)
#	ucidef_set_interface_raw "eth" "eth0"
#	ucidef_set_interfaces_lan_wan "lan1 lan2 lan3 lan4" "wan"
#	;;
#
#dir-615-i1 |\
#r6100 |\
#smart-300 |\
#tl-mr3420-v2 |\
#tl-wr841n-v8 |\
#tl-wr842n-v2 |\
#tl-wr941nd-v5 |\
#tl-wdr6500-v2 |\
#wnr2000-v3 |\
#wnr2000-v4 |\
#wnr2200 |\
#wnr612-v2 |\
#wnr1000-v2)
#	ucidef_set_interfaces_lan_wan "eth1" "eth0"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0 1 2 3 4"
#	;;
#
#uap-pro)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0t 1"
#	ucidef_add_switch_vlan "switch0" "2" "0t 2"
#	;;
#
#nanostation-m-xw)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0t 5"
#	ucidef_add_switch_vlan "switch0" "2" "0t 1"
#	;;
#
#wrt160nl)
#	ucidef_set_interfaces_lan_wan "eth0" "eth1"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0 1 2 3 4 5"
#	;;
#
#wzr-hp-g300nh)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth1"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0 1 2 3 5t"
#	;;
#
#dgl-5500-a1 |\
#dir-825-c1)
#	local mac
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0t 1 2 3 4"
#	ucidef_add_switch_vlan "switch0" "2" "0t 5"
#	mac=$(mtd_get_mac_ascii nvram "wan_mac")
#	[ -n "$mac" ] && ucidef_set_interface_macaddr "wan" "$mac"
#	;;
#
#mynet-n750)
#	local mac
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0t 1 2 3 4"
#	ucidef_add_switch_vlan "switch0" "2" "0t 5"
#	mac=$(mtd_get_mac_ascii devdata "wanmac")
#	[ -n "$mac" ] && ucidef_set_interface_macaddr "wan" "$mac"
#	;;
#
#onion-omega)
#	ucidef_set_interface_lan "wlan0"
#	;;
#
#dhp-1565-a1 |\
#dir-835-a1 |\
#wndr3700v4 | \
#wndr4300)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0t 1 2 3 4"
#	ucidef_add_switch_vlan "switch0" "2" "0t 5"
#	mac_lan=$(mtd_get_mac_binary caldata 0)
#	[ -n "$mac_lan" ] && ucidef_set_interface_macaddr "lan" "$mac_lan"
#	mac_wan=$(mtd_get_mac_binary caldata 6)
#	[ -n "$mac_wan" ] && ucidef_set_interface_macaddr "wan" "$mac_wan"
#	;;
#
#esr900)
#	local mac
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0t 1 2 3 4"
#	ucidef_add_switch_vlan "switch0" "2" "0t 5"
#	mac=$(mtd_get_mac_ascii u-boot-env "wanaddr")
#	[ -n "$mac" ] && ucidef_set_interface_macaddr "wan" "$mac"
#	;;
#
#dlan-pro-500-wp)
#	ucidef_set_interface_lan "eth0 eth1"
#	;;
#
#dlan-pro-1200-ac)
#	ucidef_set_interface_lan "eth0"
#	ucidef_add_switch "switch0" "1" "0"
#	ucidef_add_switch_vlan "switch0" "0" "0 2 3 4"
#	;;
#
#all0305 |\
#antminer-s1 |\
#antminer-s3 |\
#aw-nr580 |\
#bullet-m |\
#cap4200ag |\
#eap300v2 |\
#eap7660d |\
#el-mini |\
#loco-m-xw |\
#mr1750 |\
#mr600 |\
#mr600v2 |\
#mr900 |\
#mr900v2 |\
#rb-411 |\
#rb-911g-2hpnd |\
#rb-911g-5hpacd |\
#rb-911g-5hpnd |\
#rb-912uag-2hpnd |\
#rb-912uag-5hpnd |\
#rb-sxt2n |\
#rb-sxt5n |\
#rocket-m-xw |\
#tl-mr10u |\
#tl-mr11u |\
#tl-mr12u |\
#tl-mr13u |\
#tl-mr3020 |\
#tl-mr3040 |\
#tl-mr3040-v2 |\
#tl-wa701nd-v2 |\
#tl-wa7210n-v2 |\
#tl-wa750re |\
#tl-wa850re |\
#tl-wa830re-v2 |\
#tl-wa801nd-v2 |\
#tl-wa901nd |\
#tl-wa901nd-v2 |\
#tl-wa901nd-v3 |\
#tl-wr703n |\
#tube2h |\
#wndap360 |\
#mynet-rext |\
#wp543)
#	ucidef_set_interface_lan "eth0"
#	;;
#
#dir-505-a1)
#	ucidef_set_interface_lan "eth1"
#	;;
#
#alfa-ap96 |\
#alfa-nx |\
#ap83 |\
#gl-inet |\
#jwap003 |\
#pb42 |\
#pb44 |\
#routerstation|\
#tl-wr710n |\
#tl-wr720n-v3|\
#wpe72)
#	ucidef_set_interfaces_lan_wan "eth1" "eth0"
#	;;
#
#wpj344)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0t 3"
#	ucidef_add_switch_vlan "switch0" "2" "0t 2"
#	;;
#
#wpj531)
#	ucidef_set_interfaces_lan_wan "eth0" "eth1"
#	;;
#
#wpj558)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "5 6t"
#	ucidef_add_switch_vlan "switch0" "2" "1 6t"
#	;;
#
#ap121 |\
#ap121-mini |\
#ap96 |\
#airrouter |\
#dir-600-a1 |\
#dir-615-c1 |\
#dir-615-e1 |\
#dir-615-e4 |\
#hiwifi-hc6361 |\
#ja76pf |\
#mc-mac1200r|\
#minibox-v1 |\
#mynet-n600 |\
#oolite |\
#qihoo-c301 |\
#rb-750 |\
#rb-751 |\
#tew-632brp |\
#tew-712br |\
#tl-mr3220 |\
#tl-mr3220-v2 |\
#tl-mr3420 |\
#tl-wdr3320-v2 |\
#tl-wdr3500 |\
#tl-wr741nd |\
#tl-wr741nd-v4 |\
#tl-wr841n-v7 |\
#tl-wr841n-v9 |\
#whr-g301n |\
#whr-hp-g300n |\
#whr-hp-gn |\
#wzr-hp-ag300h)
#	ucidef_set_interfaces_lan_wan "eth0" "eth1"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0 1 2 3 4"
#	;;
#
#wzr-hp-g450h)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0t 2 3 4 5"
#	ucidef_add_switch_vlan "switch0" "2" "0t 1"
#	;;
#
#ew-dorin)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth0.2"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0t 1 2"
#	ucidef_add_switch_vlan "switch0" "2" "0t 3"
#	;;
#
#ew-dorin-router)
#	ucidef_set_interfaces_lan_wan "eth0" "eth1"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0 2 3"
#	;;
#
#wndr3700)
#	ucidef_set_interfaces_lan_wan "eth0.1" "eth1"
#	ucidef_add_switch "switch0" "1" "1"
#	ucidef_add_switch_vlan "switch0" "1" "0 1 2 3 5t"
#	ucidef_add_switch_port "switch0" "1"
#
#*)
#	ucidef_set_interfaces_lan_wan "eth0" "eth1"
#	;;
esac

# shall I commit changes? Yes, when called by hand.
if [ ${AUTOCOMMIT} == "yes" ];  then
	echo 'uci commit network';
	uci commit network;
	/etc/init.d/network restart
	else 
	echo 'uci dont commit network'
	
fi

exit 0
