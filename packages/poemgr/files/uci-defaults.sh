#!/bin/sh

[ -e /etc/config/poemgr ] && exit 0

. /lib/functions/uci-defaults.sh

board=$(board_name)
case "$board" in
ubnt,usw-flex)
    cp /usr/lib/poemgr/usw-flex.config /etc/config/poemgr
    ;;
esac
