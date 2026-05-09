#!/usr/bin/env bash

iidfile=/tmp/falter-packages-image-id.txt
podman build --iidfile=$iidfile build/
img=$(cat $iidfile)
rm -f $iidfile

echo
echo "Executing build/build.sh $@"
echo
podman run -it --rm --log-driver=none -v $(pwd):/work:Z --userns=keep-id "$img" "$@"
