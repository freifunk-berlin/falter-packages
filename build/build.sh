#!/usr/bin/env bash

# TODO: <destdir>/build.log
# TODO: <destdir>/feeds.conf
# TODO: print only to stderr
# TODO: container tar output

# podman run -i --rm alpine:edge sh -c '( apk add git bash wget xz coreutils build-base gcc abuild binutils ncurses-dev gawk bzip2 perl python3 rsync && git clone https://github.com/pktpls/falter-packages.git /root/falter-packages && cd /root/falter-packages/ && git checkout build && build/build.sh master x86_64 out/ ) >&2 && cd /root/falter-packages/out/ && tar -c *' > out.tar

set -ex
set -o pipefail

function usage() {
  echo "usage: build/build.sh <branch> <arch> <destination>" >&2
  exit 1
}

[ -n "$1" ] && branch="$1" || usage
[ -n "$2" ] && arch="$2" || usage
[ -n "$3" ] && dest="$3" || usage

destdir="$dest/snapshots/packages/$arch"
[ "$branch" == "openwrt-22.03" ] && destdir="$dest/packages-22.03/$arch"
[ "$branch" == "openwrt-21.02" ] && destdir="$dest/packages-21.02/$arch"
mkdir -p "$destdir"
destdir=$(realpath "$destdir")

sdkdir="./tmp/$branch/$arch"

# the grep pipes at the end would otherwise be buffered
unbuf="stdbuf --output=0 --error=0"
(
  mirror="https://downloads.openwrt.org"
  #mirror="file:///mnt/mirror/downloads.openwrt.org"
  mirrordir="snapshots"
  [ "$branch" == "openwrt-22.03" ] && mirrordir="releases/22.03-SNAPSHOT"
  [ "$branch" == "openwrt-21.02" ] && mirrordir="releases/21.02-SNAPSHOT"

  target=$(cat "./build/targets-$branch.txt" | grep -v '#' | grep -F "$arch" | cut -d ' ' -f 2)
  sdk=$(curl -s "$mirror/$mirrordir/targets/$target/sha256sums" | cut -d '*' -f 2 | grep -i sdk)

  mkdir -p "./tmp/dl/$branch"
  wget --progress=dot:giga -O "./tmp/dl/$branch/$sdk" "$mirror/$mirrordir/targets/$target/$sdk"

  rm -rf "$sdkdir"
  mkdir -p "$sdkdir"
  tar -x -C "$sdkdir" --strip-components=1 -f "./tmp/dl/$branch/$sdk"

  mkdir -p ./tmp/feed
  ln -sfT "$(pwd)/packages" ./tmp/feed/packages
  ln -sfT "$(pwd)/luci" ./tmp/feed/luci
  cp "$sdkdir/feeds.conf.default" "$sdkdir/feeds.conf"
  echo "src-link falter $(pwd)/tmp/feed" >> "$sdkdir/feeds.conf"

  cd "$sdkdir"

  echo "CONFIG_SIGNED_PACKAGES=n" > .config
  make defconfig
  ./scripts/feeds update -a
  ./scripts/feeds install -a -p falter
  for p in $(find -L feeds/falter -name Makefile | awk -F/ '{print $(NF - 1)}'); do
    make -j8 V=s "package/$p/compile"
  done
  make package/index V=s
) \
  |& $unbuf grep -v 'warning: ignoring type redefinition' \
  | $unbuf grep -v 'warning: defaults for choice' \
  | $unbuf grep -v "linux/Makefile' has a dependency" \
  | tee "$destdir/build.log" \
  >&2

rm -rf "$destdir/falter"
mv "$sdkdir/bin/packages/$arch/falter" "$destdir/"
