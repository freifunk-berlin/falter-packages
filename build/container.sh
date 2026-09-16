#!/usr/bin/env bash

iidfile=./.tmp-falter-image-id.txt
podman build --iidfile=$iidfile --pull=newer --network=host build/
img=$(cat $iidfile)
rm -f $iidfile

echo
echo "Executing build/build.sh $*"
echo
podman run -it --rm --log-driver=none -v "$(pwd):/work:Z" --userns=keep-id \
  -e OPENWRT_MIRROR -e FALTER_MIRROR -e FALTER_VARIANT -e FALTER_FEED -e FALTER_FEEDKEY \
  "$img" "$@"
