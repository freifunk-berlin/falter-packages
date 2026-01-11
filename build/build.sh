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
    echo "  main  openwrt-25.12  openwrt-24.10  openwrt-23.05  openwrt-22.03"
    echo
    echo "arch names:"
    echo "  run build/build.sh with a branch name to see available arch names"
  fi
  echo
  echo "destination:"
  echo "  path to a writable directory where the 'falter' feed directory will end up."
  echo "  default: ./out/<branch>/<arch>"
  echo
  echo "FALTER_MIRROR env variable:"
  echo "  sets the base URL of a mirror which serves copies of downloads.openwrt.org and firmware.berlin.freifunk.net."
  echo "  default: <empty>"
  echo
  echo "FALTER_DEBUG env variable:"
  echo "  set to any non-empty value (e.g. 1) to include debug symbols in binaries."
  echo "  default: <empty>"
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
  gitmirror="https://github.com"
else
  dlmirror="$FALTER_MIRROR/downloads.openwrt.org"
  srcmirror="$FALTER_MIRROR/sources.openwrt.org"
  gitmirror="$FALTER_MIRROR/git.openwrt.org"
fi

makeargs="V=s"
[ -z "$FALTER_DEBUG" ] || makeargs="$makeargs CONFIG_DEBUG=y STRIP=true"

mkdir -p "$dest/falter"
destdir=$(realpath "$dest")

sdkdir="./tmp/$branch/$arch"

# the grep pipes at the end would otherwise be buffered
unbuf="stdbuf --output=0 --error=0"
(
  # pick the right URL
  dlurl="$dlmirror/snapshots/targets"
  [ "$branch" == "openwrt-25.12" ] && dlurl="$dlmirror/releases/25.12-SNAPSHOT/targets"
  [ "$branch" == "openwrt-24.10" ] && dlurl="$dlmirror/releases/24.10-SNAPSHOT/targets"
  [ "$branch" == "openwrt-23.05" ] && dlurl="$dlmirror/releases/23.05.6/targets"
  [ "$branch" == "openwrt-22.03" ] && dlurl="$dlmirror/releases/22.03.7/targets"
  [ "$branch" == "openwrt-21.02" ] && dlurl="$dlmirror/releases/21.02.7/targets"

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

  owbranch="main"
  owbranch2="master"
  [ "$branch" == "main" ] || [ "$branch" == "testbuildbot" ] || owbranch="$branch"
  [ "$branch" == "main" ] || [ "$branch" == "testbuildbot" ] || owbranch2="$branch"
  if [ "$gitmirror" == "https://git.openwrt.org" ]; then
    cat <<EOF >"$sdkdir/feeds.conf"
src-git base https://git.openwrt.org/openwrt/openwrt.git;$owbranch
src-git packages https://git.openwrt.org/feed/packages.git;$owbranch2
src-git luci https://git.openwrt.org/project/luci.git;$owbranch2
src-git routing https://git.openwrt.org/feed/routing.git;$owbranch2
src-git telephony https://git.openwrt.org/feed/telephony.git;$owbranch2
src-link falter $(pwd)/tmp/feed
EOF
  elif [ "$gitmirror" == "https://github.com" ] ; then
    cat <<EOF >"$sdkdir/feeds.conf"
src-git base https://github.com/openwrt/openwrt.git;$owbranch
src-git packages https://github.com/openwrt/packages.git;$owbranch2
src-git luci https://github.com/openwrt/luci.git;$owbranch2
src-git routing https://github.com/openwrt/routing.git;$owbranch2
src-git telephony https://github.com/openwrt/telephony.git;$owbranch2
src-link falter $(pwd)/tmp/feed
EOF
  else
    cat <<EOF >"$sdkdir/feeds.conf"
src-git-full base $gitmirror/openwrt/openwrt.git;$owbranch
src-git-full packages $gitmirror/feed/packages.git;$owbranch2
src-git-full luci $gitmirror/project/luci.git;$owbranch2
src-git-full routing $gitmirror/feed/routing.git;$owbranch2
src-git-full telephony $gitmirror/feed/telephony.git;$owbranch2
src-link falter $(pwd)/tmp/feed
EOF
  fi

  cd "$sdkdir"

  # build a repository from all packages in our feed
  if [ "$branch" != "main" ] && [ "$branch" != "openwrt-25.12" ] ; then
    echo "CONFIG_SIGNED_PACKAGES=n" > .config
  fi
  make defconfig
  ./scripts/feeds update -a
  ./scripts/feeds install -a -p falter

  sed -i 's#cc -o contrib/lemon#cc -std=gnu17 -o contrib/lemon#g' feeds/luci/modules/luci-base/src/Makefile

  export DOWNLOAD_MIRROR="$srcmirror"
  for p in $(find -L feeds/falter -name Makefile | awk -F/ '{print $(NF - 1)}' | sort); do
    cmd="make package/$p/compile $makeargs"
    echo "-- $cmd"
    $cmd
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
