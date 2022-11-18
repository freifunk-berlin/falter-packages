# Installation

This tutorial gives a short view in how you could incorporate falter-packages, if you compile all of your OpenWrt-images by yourself. This is not the preffered method though. The falter project prefers using OpenWrts SDK and imagebuilders, to get full binary compatibility to OpenWrt.

1. Clone OpenWrt

```sh
git clone https://git.openwrt.org/openwrt/openwrt.git
```

2. Change into OpenWrt folder by

```sh
cd openwrt
```

3. Copy `feeds.conf.default` to `feeds.conf`

```sh
cp feeds.conf.default feeds.conf
```

4. after that add to feeds.conf

```sh
src-git falter https://github.com/freifunk-berlin/falter-packages.git
```

5. Then do

```sh
./scripts/feeds update -a
./scripts/feeds install -a
```

6. Select packages using

```sh
make menuconfig
```

7. Start compiling your custom OpenWrt:

```sh
make
```
