#!/bin/sh
#

# if autoupdate is not present in crontab, include it.
crontab -l | grep /usr/bin/autoupdate >>/dev/null
if [ $? != 0 ]; then
    # get a fairly random update-time, to protect the servers from DoS. Will be something between 3 and 5 a.m.
    HOUR=$((($(tr -cd 0-9 </dev/urandom | head -c 2) % 3) + 3))
    MIN=$(($(tr -cd 0-9 </dev/urandom | head -c 2) % 60))
    echo "$MIN $HOUR * * *        /usr/bin/autoupdate" >>/etc/crontabs/root

    /etc/init.d/cron restart
fi
