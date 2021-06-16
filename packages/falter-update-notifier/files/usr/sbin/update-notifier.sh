#!/bin/ash

# check if there is a newer version on https://firmware.berlin.freifunk.net/stable

. /etc/freifunk_release

RECENT_VERSION=$(wget -q https://firmware.berlin.freifunk.net/stable/ -O - | grep '<a href="' | cut -d' ' -f2 | cut -d'"' -f2 | cut -d'/' -f1 | tail -n 1)

# https://stackoverflow.com/a/52707989
if [ $(expr x"$RECENT_VERSION" \> x"$FREIFUNK_RELEASE") = 1 ]; then
    grep "Update available" /etc/profile.d/dynbanner.sh > /dev/null
    if [ $? -ne 0 ]; then
    # set update-flag in freifunk_releases for luci-gui
    echo 'UPDATE_AVAILABLE="true"' >> /etc/freifunk_release
    # add notification to dynbanner-text. User will get notification on every login
    printf '
printf "
+++ Update available! +++

A new version of Freifunk Berlin firmware is available!
Get important security updates and great new features at:

https://selector.berlin.freifunk.net

Please check the Freifunk Berlin mailing-list for further
information on the update and update soon.\n\n"\n' >> /etc/profile.d/dynbanner.sh
    fi
fi
