#!/bin/sh

function next_port {
	local port_start=$1
	local port_end=$2

	ports=$(wg show all listen-port | awk '{print $2}')

	# assume for now only 1 value @[0]
	#port_start=$(uci get wgserver.@server[0].port_start)
	#port_end=$(uci get wgserver.@server[0].port_end)

	for i in $(seq $port_start $port_end); do
		if ! echo $ports | grep -q "$i"; then
			echo $i
			return
		fi
	done
}
