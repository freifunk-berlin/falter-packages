#!/usr/bin/env bash

iidfile=/tmp/falter-packages-image-id.txt
podman build --iidfile=$iidfile --pull=newer build/
img=$(cat $iidfile)
rm -f $iidfile

echo
echo "Executing build/build.sh $@"
echo
podman run -it --rm --log-driver=none -v $(pwd):/work:Z --userns=keep-id \
  -e OPENWRT_MIRROR -e FALTER_MIRROR -e GIT_MIRROR -e SOURCES_MIRROR -e FALTER_DEBUG \
  "$img" "$@"
