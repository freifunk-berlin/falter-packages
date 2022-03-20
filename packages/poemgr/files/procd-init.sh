#!/bin/sh /etc/rc.common

START=80
USE_PROCD=1

NAME=poemgr
PROG=/sbin/poemgr

. /lib/functions.sh


reload_service() {
	start
}

service_triggers() {
	procd_add_reload_trigger poemgr
}

stop_service()
{
	$PROG disable
}

start_service()
{
	DISABLED="$(uci -q get poemgr.settings.disabled)"
	DISABLED="${DISABLED:-0}"

	if [ "$DISABLED" -gt 0 ]
	then
		$PROG disable
	else
		$PROG apply
	fi
}
