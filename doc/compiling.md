# Compiling the Falter Packages

## Some Backround Information on Structure

In the falter-packages repo, you will find a `build/` directory containing at least four files:

```sh
$ ls build
build.sh
targets-master.txt
targets-openwrt-21.02.txt
targets-openwrt-22.03.txt
```

Whereas `build.sh` is the script for generating the whole package feed and the other files contain the mapping from a cpu-architecture (like `mips_24kc`) to a target (like `ath79/generic`) per OpenWrt-Version.

Theses target-mappings can be generated automatically by the OpenWrt build system. There you need to check out the branch of the correspondent OpenWrt-Version and run:

```sh
$ scripts/dump-target-info.pl architectures
aarch64_cortex-a53 armvirt/64 bcm27xx/bcm2710 bcm4908/generic mediatek/mt7622 mvebu/cortexa53 sunxi/cortexa53
aarch64_cortex-a72 bcm27xx/bcm2711 mvebu/cortexa72
aarch64_generic layerscape/armv8_64b octeontx/generic rockchip/armv8
arc_archs archs38/generic
arm_arm1176jzf-s_vfp bcm27xx/bcm2708
arm_arm926ej-s at91/sam9x mxs/generic
[...]
```

The data retrieved there needs to be pasted into the falters repo `build/` directory. The files get named in a certain scheme: `targets-${FALTERBRANCH}.txt`. If you meet that convention, the build-script will be able to find the right cpu-architecture for the requested target.

## Compiling Packages

Compiling the packages works by checking out the branch, that we want to build and running the build-script:

```sh
    usage: build/build.sh <branch> <arch> <destination>
```

One example call for building packages compatible for `ath79/generic` works like this:

```sh
$ build/build.sh openwrt-21.02 mips_24kc out/

--2022-11-18 10:43:32--  https://downloads.openwrt.org/releases/21.02-SNAPSHOT/targets/ath79/generic/openwrt-sdk-21.02-SNAPSHOT-ath79-generic_gcc-8.4.0_musl.Linux-x86_64.tar.xz
Auflösen des Hostnamens downloads.openwrt.org (downloads.openwrt.org)… 168.119.138.211, 2a01:4f8:251:321::2
Verbindungsaufbau zu downloads.openwrt.org (downloads.openwrt.org)|168.119.138.211|:443 … verbunden.
HTTP-Anforderung gesendet, auf Antwort wird gewartet … 200 OK
Länge: 101628600 (97M) [application/octet-stream]
Wird in »./tmp/dl/openwrt-21.02/openwrt-sdk-21.02-SNAPSHOT-ath79-generic_gcc-8.4.0_musl.Linux-x86_64.tar.xz« gespeichert.

     0K ........ ........ ........ ........ 33% 7,89M 8s
 32768K ........ ........ ........ ........ 66% 8,12M 4s
 65536K ........ ........ ........ ........ 99% 7,50M 0s
 98304K                                    100% 7,98M=12s

2022-11-18 10:43:44 (7,83 MB/s) - »./tmp/dl/openwrt-21.02/openwrt-sdk-21.02-SNAPSHOT-ath79-generic_gcc-8.4.0_musl.Linux-x86_64.tar.xz« gespeichert [101628600/101628600]

Checking 'working-make'... ok.
Checking 'case-sensitive-fs'... ok.
Checking 'proper-umask'... ok.
[...]
```

The script will download the OpenWrt-SDK for the specific architecture and compile all packages in the feed. This works for local packages, that aren't checked into git too.

Please mind, that this will build the packagefeed, _but won't sign it_. Signing needs to be handled separately (i.e. automatic builds get signed on buildbot master).
