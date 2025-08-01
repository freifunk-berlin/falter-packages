#!/usr/bin/env bash

# shellcheck disable=SC2015

# TODO: generate feeds.conf with revision info
# TODO: use $dlmirror for repositories.conf as well

# To run this in a rootless podman container:
#   podman run -i --rm --timeout=1800 --log-driver=none alpine:edge sh -c '( apk add git bash wget xz coreutils build-base gcc argp-standalone musl-fts-dev musl-obstack-dev musl-libintl abuild binutils ncurses-dev gawk bzip2 perl python3 rsync && git clone https://github.com/freifunk-berlin/falter-packages.git /root/falter-packages && cd /root/falter-packages/ && git checkout main && build/build.sh main x86_64 out/ ) >&2 && cd /root/falter-packages/out/ && tar -c *' > out.tar

function usage() {
  local br="$1"
  local arch=""

  echo "usage: build/build.sh <branch> <arch> [<destination>]"
  echo
  if [ -n "$br" ]; then
    echo "branch name:"
    echo "  $br"
    echo
    echo "arch names:"
    (grep -v '#' | grep . | cut -d' ' -f1) < "build/targets-$br.txt" | while IFS= read -r arch
    do
      echo -n "  $arch"
    done
    echo
  else
    echo "branch names:"
    echo "  main  openwrt-24.10  openwrt-23.05  openwrt-22.03"
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

# shell check SC2015: If-then-else work here, as a assignment can not fail.
[ -n "$1" ] && branch="$1" || usage >&2
[ -n "$2" ] && arch="$2" || usage "$branch" >&2
[ -n "$3" ] && dest="$3" || dest="./out/$branch/$arch"

set -o pipefail
set -e
set -x

if [ -z "$FALTER_MIRROR" ] ; then
  dlmirror="https://downloads.openwrt.org"
  srcmirror="https://sources.openwrt.org"
  gitmirror="https://git.openwrt.org"
else
  dlmirror="$FALTER_MIRROR/downloads.openwrt.org"
  srcmirror="$FALTER_MIRROR/sources.openwrt.org"
  gitmirror="$FALTER_MIRROR/git.openwrt.org"
fi

mkdir -p "$dest/falter"
destdir=$(realpath "$dest")

sdkdir="./tmp/$branch/$arch"

# the grep pipes at the end would otherwise be buffered
unbuf="stdbuf --output=0 --error=0"
(
  # pick the right URL
  dlurl="$dlmirror/snapshots/targets"
  [ "$branch" == "openwrt-24.10" ] && dlurl="$dlmirror/releases/24.10-SNAPSHOT/targets"
  [ "$branch" == "openwrt-23.05" ] && dlurl="$dlmirror/releases/23.05-SNAPSHOT/targets"
  [ "$branch" == "openwrt-22.03" ] && dlurl="$dlmirror/releases/22.03-SNAPSHOT/targets"
  [ "$branch" == "openwrt-21.02" ] && dlurl="$dlmirror/releases/21.02-SNAPSHOT/targets"

  # determine the sdk tarball's filename
  target=$( (grep -v '#' | grep -F "$arch " | cut -d ' ' -f 2) < "./build/targets-$branch.txt")
  sdkfile=$(wget -q -O - "$dlurl/$target/sha256sums" | cut -d '*' -f 2 | grep -i openwrt-sdk-)

  # download and extract sdk tarball
  mkdir -p "./tmp/dl/$branch"
  wget -nv -N -P "./tmp/dl/$branch" "$dlurl/$target/$sdkfile"
  rm -rf "$sdkdir"
  mkdir -p "$sdkdir"
  tar -x -C "$sdkdir" --strip-components=1 -f "./tmp/dl/$branch/$sdkfile"

  # configure our feed, with an indirection via /tmp/feed so sdk doesn't recurse our feed
  mkdir -p ./tmp/feed
  ln -sfT "$(pwd)/packages" ./tmp/feed/packages
  ln -sfT "$(pwd)/luci" ./tmp/feed/luci
  branchmaster="master"
  [ "$branch" == "main" ] || branchmaster="$branch"
  if [ "$gitmirror" == "https://git.openwrt.org" ]; then
    cat <<EOF >"$sdkdir/feeds.conf"
src-git base https://git.openwrt.org/openwrt/openwrt.git;$branch
src-git packages https://git.openwrt.org/feed/packages.git;$branchmaster
src-git luci https://git.openwrt.org/project/luci.git;$branchmaster
src-git routing https://git.openwrt.org/feed/routing.git;$branchmaster
src-git telephony https://git.openwrt.org/feed/telephony.git;$branchmaster
src-link falter $(pwd)/tmp/feed
EOF
  elif [ "$gitmirror" == "https://github.com" ] ; then
    cat <<EOF >"$sdkdir/feeds.conf"
src-git base https://github.com/openwrt/openwrt.git;$branch
src-git packages https://github.com/openwrt/packages.git;$branchmaster
src-git luci https://github.com/openwrt/luci.git;$branchmaster
src-git routing https://github.com/openwrt/routing.git;$branchmaster
src-git telephony https://github.com/openwrt/telephony.git;$branchmaster
src-link falter $(pwd)/tmp/feed
EOF
  else
    cat <<EOF >"$sdkdir/feeds.conf"
src-git-full base $gitmirror/openwrt/openwrt.git;$branch
src-git-full packages $gitmirror/feed/packages.git;$branchmaster
src-git-full luci $gitmirror/project/luci.git;$branchmaster
src-git-full routing $gitmirror/feed/routing.git;$branchmaster
src-git-full telephony $gitmirror/feed/telephony.git;$branchmaster
src-link falter $(pwd)/tmp/feed
EOF
  fi

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

[ ! -f "$sdkdir/public-key.pem" ] || mv "$sdkdir/public-key.pem" "$destdir/"
mv "$sdkdir/bin/packages/$arch"/falter/* "$destdir/falter/"
