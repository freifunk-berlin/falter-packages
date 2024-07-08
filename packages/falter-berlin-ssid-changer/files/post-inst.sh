#!/bin/sh

[ -z $IPKG_INSTROOT ] || exit 0

# if ssid-changer is not present in crontab, include it.
crontab -l | grep ssid-changer >>/dev/null
if [ $? != 0 ]; then
    # check every 5 minutes, if we can still reach the internet, but delay
    # execution randomly, to achieve evenly distribution
    echo '# check every 5 minutes, if we can still reach the internet, but delay execution randomly, to achieve evenly distribution' >>/etc/crontabs/root
    echo '*/5 * * * *      test -e /etc/hotplug.d/net/30-ssid-changer && DELAY=some INTERFACE=ffuplink sh /etc/hotplug.d/net/30-ssid-changer' >>/etc/crontabs/root

    /etc/init.d/cron restart
fi
