#!/usr/bin/env bash

# TODO: generate feeds.conf with revision info
# TODO: use $dlmirror for repositories.conf as well

# To run this in a rootless podman container:
#   podman run -i --rm --timeout=1800 --log-driver=none alpine:edge sh -c '( apk add git bash wget xz coreutils build-base gcc argp-standalone musl-fts-dev musl-obstack-dev musl-libintl abuild binutils ncurses-dev gawk bzip2 perl python3 rsync && git clone https://github.com/freifunk-berlin/falter-packages.git /root/falter-packages && cd /root/falter-packages/ && git checkout master && build/build.sh master x86_64 out/ ) >&2 && cd /root/falter-packages/out/ && tar -c *' > out.tar

function usage() {
  local br="$1"
  echo "usage: build/build.sh <branch> <arch> [<destination>]"
  echo
  if [ -n "$br" ]; then
    echo "branch name:"
    echo "  $br"
    echo
    echo "arch names:"
    for a in $(cat "build/targets-$br.txt" | grep -v '#' | grep . | cut -d' ' -f1); do
      echo -n "  $a"
    done
    echo
  else
    echo "branch names:"
    echo "  master  openwrt-22.03  openwrt-21.02"
    echo
    echo "arch names:"
    echo "  run build/build.sh with a branch name to see available arch names"
  fi
  echo
  echo "destination:"
  echo "  path to a writable directory where the 'falter' feed directory will end up."
  echo "  default: ./out/<branch>/<arch>"
  echo
  exit 1
}

[ -n "$1" ] && branch="$1" || usage >&2
[ -n "$2" ] && arch="$2" || usage "$branch" >&2
[ -n "$3" ] && dest="$3" || dest="./out/$branch/$arch"

set -o pipefail
set -e
set -x

# Mirror base URL for SDK download.
# We search in $dlmirror/releases/$version-SNAPSHOT/targets/sha256sum.
dlmirror="https://downloads.openwrt.org"
# dlmirror="file:///mnt/mirror/downloads.openwrt.org"
# dlmirror="http://192.168.1.1/downloads.openwrt.org"

# Mirror URL for source tarball downloads.
srcmirror="https://sources.openwrt.org;https://firmware.berlin.freifunk.net/sources"
# srcmirror="file:///mnt/mirror/sources.openwrt.org;file:///mnt/mirror/firmware.berlin.freifunk.net/sources"
# srcmirror="http://192.168.1.1/sources.openwrt.org;http://192.168.1.1/firmware.berlin.freifunk.net/sources"

# Mirror URL for Git repositories in feeds.conf
gitmirror="https://git.openwrt.org"
# gitmirror="file:///mnt/mirror/git.openwrt.org"
# gitmirror="http://192.168.1.1/git.openwrt.org"

mkdir -p "$dest/falter"
destdir=$(realpath "$dest")

sdkdir="./tmp/$branch/$arch"

# the grep pipes at the end would otherwise be buffered
unbuf="stdbuf --output=0 --error=0"
(
  # pick the right URL
  dlurl="$dlmirror/snapshots/targets"
  [ "$branch" == "openwrt-22.03" ] && dlurl="$dlmirror/releases/22.03-SNAPSHOT/targets"
  [ "$branch" == "openwrt-21.02" ] && dlurl="$dlmirror/releases/21.02-SNAPSHOT/targets"

  # determine the sdk tarball's filename
  target=$(cat "./build/targets-$branch.txt" | grep -v '#' | grep -F "$arch " | cut -d ' ' -f 2)
  sdkfile=$(wget -q -O - "$dlurl/$target/sha256sums" | cut -d '*' -f 2 | grep -i openwrt-sdk-)

  # download and extract sdk tarball
  mkdir -p "./tmp/dl/$branch"
  wget --progress=dot:giga -O "./tmp/dl/$branch/$sdkfile" "$dlurl/$target/$sdkfile"
  rm -rf "$sdkdir"
  mkdir -p "$sdkdir"
  tar -x -C "$sdkdir" --strip-components=1 -f "./tmp/dl/$branch/$sdkfile"

  # configure our feed, with an indirection via /tmp/feed so sdk doesn't recurse our feed
  mkdir -p ./tmp/feed
  ln -sfT "$(pwd)/packages" ./tmp/feed/packages
  ln -sfT "$(pwd)/luci" ./tmp/feed/luci
  cp "$sdkdir/feeds.conf.default" "$sdkdir/feeds.conf"
  if [ "$gitmirror" != "https://git.openwrt.org" ]; then
    sed -i "s|https://git.openwrt.org/openwrt|$gitmirror|g" "$sdkdir/feeds.conf"
    sed -i "s|https://git.openwrt.org/feed|$gitmirror|g" "$sdkdir/feeds.conf"
    sed -i "s|https://git.openwrt.org/project|$gitmirror|g" "$sdkdir/feeds.conf"
    sed -i 's|src-git |src-git-full |g' "$sdkdir/feeds.conf"
  fi
  echo "src-link falter $(pwd)/tmp/feed" >> "$sdkdir/feeds.conf"

  cd "$sdkdir"

  # build a repository from all packages in our feed
  echo "CONFIG_SIGNED_PACKAGES=n" > .config
  make defconfig
  ./scripts/feeds update -a
  ./scripts/feeds install -a -p falter
  export DOWNLOAD_MIRROR="$srcmirror"
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
