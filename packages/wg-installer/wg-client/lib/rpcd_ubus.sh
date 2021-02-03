#!/bin/sh

# This script is for communicating with the gateway.

. /usr/share/libubox/jshn.sh

# Copied from:
# https://stackoverflow.com/questions/31457586/how-can-i-check-ip-version-4-or-6-in-shell-script
function isipv6 {
	local ip=$1
	# I do not like this bash behavior
	if [ "$ip" != "${1#*[0-9].[0-9]}" ]; then
		return 1
	elif [ "$ip" != "${1#*:[0-9a-fA-F]}" ]; then
		return 0
	fi
	return 1
}

function request_token {
	local ip=$1
	local user=$2
	local password=$3

	json_init
	json_add_string "jsonrpc" "2.0"
	json_add_int "id" "1"
	json_add_string "method" "call"
	json_add_array "params"
	json_add_string "" "00000000000000000000000000000000"
	json_add_string "" "session"
	json_add_string "" "login"
	json_add_object
	json_add_string "username" $user
	json_add_string "password" $password
	json_close_object
	json_close_array
	req=$(json_dump)
	ret=$(curl --insecure https://$ip/ubus -d "$req") 2>/dev/null
	if [ $? != 0 ]; then
		return 1
	fi
	json_load "$ret"
	json_get_vars result result
	json_select result
	json_select 2
	json_get_var ubus_rpc_session ubus_rpc_session
	echo $ubus_rpc_session
}

function wg_rpcd_get_usage {
	local token=$1
	local ip=$2
	local secret=$3

	json_init
	json_add_string "jsonrpc" "2.0"
	json_add_int "id" "1"
	json_add_string "method" "call"
	json_add_array "params"
	json_add_string "" $token
	json_add_string "" "wginstaller"
	json_add_string "" "get_usage"
	json_add_object
	json_close_object
	json_close_array
	req=$(json_dump)
	ret=$(curl --insecure https://$ip/ubus -d "$req") 2>/dev/null
	if [ $? != 0 ]; then
		return 1
	fi

	# return values
	json_load "$ret"
	json_get_vars result result
	json_select result
	json_select 2
	json_get_var num_interfaces num_interfaces
	echo "num_interfaces: ${num_interfaces}"
}

function wg_rpcd_register {
	local token=$1
	local ip=$2
	local uplink_bw=$3
	local mtu=$4
	local public_key=$5

	json_init
	json_add_string "jsonrpc" "2.0"
	json_add_int "id" "1"
	json_add_string "method" "call"
	json_add_array "params"
	json_add_string "" $token
	json_add_string "" "wginstaller"
	json_add_string "" "register"
	json_add_object
	json_add_int "uplink_bw" $uplink_bw
	json_add_int "mtu" $mtu
	json_add_string "public_key" $public_key
	json_close_object
	json_close_array
	req=$(json_dump)
	ret=$(curl --insecure https://$ip/ubus -d "$req") 2>/dev/null
	if [ $? != 0 ]; then
		return 1
	fi

	json_load "$ret"
	json_get_vars result result
	json_select result
	json_select 2
	json_get_var pubkey pubkey
	json_get_var gw_ip gw_ip
	json_get_var port port
	echo "pubkey: ${pubkey}"
	echo "gw_ip: ${gw_ip}"
	echo "port: ${port}"
}
