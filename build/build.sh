#!/usr/bin/env bash

# TODO: generate feeds.conf with revision info

# To run this in a rootless podman container:
#   podman run -i --rm --timeout=1800 --log-driver=none alpine:edge sh -c '( apk add git bash wget xz coreutils build-base gcc argp-standalone musl-fts-dev musl-obstack-dev musl-libintl abuild binutils ncurses-dev gawk bzip2 perl python3 rsync && git clone https://github.com/freifunk-berlin/falter-packages.git /root/falter-packages && cd /root/falter-packages/ && git checkout master && build/build.sh master x86_64 out/ ) >&2 && cd /root/falter-packages/out/ && tar -c *' > out.tar

set -ex
set -o pipefail

function usage() {
  echo "usage: build/build.sh <branch> <arch> <destination>" >&2
  exit 1
}

dlmirror="https://downloads.openwrt.org"
#dlmirror="file:///mnt/mirror/downloads.openwrt.org"

[ -n "$1" ] && branch="$1" || usage
[ -n "$2" ] && arch="$2" || usage
[ -n "$3" ] && dest="$3" || usage

destdir=$(realpath "$dest")
mkdir -p "$destdir/falter"

sdkdir="./tmp/$branch/$arch"

# the grep pipes at the end would otherwise be buffered
unbuf="stdbuf --output=0 --error=0"
(
  # pick the right URL
  dldir="snapshots"
  [ "$branch" == "openwrt-22.03" ] && dldir="releases/22.03-SNAPSHOT"
  [ "$branch" == "openwrt-21.02" ] && dldir="releases/21.02-SNAPSHOT"

  # determine the sdk tarball's filename
  target=$(cat "./build/targets-$branch.txt" | grep -v '#' | grep -F "$arch " | cut -d ' ' -f 2)
  sdk=$(wget -q -O - "$dlmirror/$dldir/targets/$target/sha256sums" | cut -d '*' -f 2 | grep -i openwrt-sdk-)

  # download and extract sdk tarball
  mkdir -p "./tmp/dl/$branch"
  wget --progress=dot:giga -O "./tmp/dl/$branch/$sdk" "$dlmirror/$dldir/targets/$target/$sdk"
  rm -rf "$sdkdir"
  mkdir -p "$sdkdir"
  tar -x -C "$sdkdir" --strip-components=1 -f "./tmp/dl/$branch/$sdk"

  # configure our feed, with an indirection via /tmp/feed so sdk doesn't recurse our feed
  mkdir -p ./tmp/feed
  ln -sfT "$(pwd)/packages" ./tmp/feed/packages
  ln -sfT "$(pwd)/luci" ./tmp/feed/luci
  cp "$sdkdir/feeds.conf.default" "$sdkdir/feeds.conf"
  echo "src-link falter $(pwd)/tmp/feed" >> "$sdkdir/feeds.conf"

  cd "$sdkdir"

  # build a repository from all packages in our feed
  echo "CONFIG_SIGNED_PACKAGES=n" > .config
  make defconfig
  ./scripts/feeds update -a
  ./scripts/feeds install -a -p falter
  for p in $(find -L feeds/falter -name Makefile | awk -F/ '{print $(NF - 1)}'); do
    make -j8 V=s "package/$p/compile"
  done
  make package/index V=s

  # filter out the useless noise before printing/logging
) \
  |& $unbuf grep -v 'warning: ignoring type redefinition' \
  | $unbuf grep -v 'warning: defaults for choice' \
  | $unbuf grep -v "linux/Makefile' has a dependency" \
  | tee "$destdir/build.log" \
  >&2

mv "$sdkdir/bin/packages/$arch"/falter/* "$destdir/falter/"
