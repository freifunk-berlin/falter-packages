#!/bin/sh
#

#if autoupdate is not present in crontab, include it.
crontab -l | grep /usr/bin/autoupdate >>/dev/null
if [ $? != 0 ]; then
    #get a fairly random update-time, to protect the servers from DoS
    HOUR=$(($(tr -cd 0-9 </dev/urandom | head -c 2) % 24))
    MIN=$(($(tr -cd 0-9 </dev/urandom | head -c 2) % 60))
    echo "$MIN $HOUR * * 4        /usr/bin/autoupdate -a" >>/etc/crontabs/root
    echo "$MIN $HOUR * * 1        /usr/bin/autoupdate -s" >>/etc/crontabs/root
    /etc/init.d/cron restart
fi


# get precise time from ntp-server
NTP_HOST=$(uci get system.ntp.server | cut -d' ' -f 1)
ntpclient -s -c 1 -h $NTP_HOST

if [ $? != 0 ]; then
    exit 1
fi


#set LAST_UPGR to $TODAY. /etc/init.d/sysfixtime should alreay have set the correct date.
if [ -e /etc/config/autoupdate ]; then
    TODAY=$(date +%s)
    uci set autoupdate.internal.last_upgr="$TODAY"
    uci commit autoupdate
else
    exit 1
fi

exit 0
