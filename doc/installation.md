# Installation

Clone OpenWrt

    git clone https://git.openwrt.org/openwrt/openwrt.git

Change into OpenWrt folder by

    cd openwrt

Copy `feeds.conf.default` to `feeds.conf`

    cp feeds.conf.default feeds.conf

after that add to feeds.conf

    src-git falter https://github.com/Freifunk-Spalter/packages.git

Then do

    ./scripts/feeds update -a
    ./scripts/feeds install -a

Select packages using

    make menuconfig