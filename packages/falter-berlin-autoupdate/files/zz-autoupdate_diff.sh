#!/bin/ash

# After everything on the router was configured, this script gets run for gene-
# rating checksums of all config files. Autoupdate uses this, to detect custom
# setups and refuse an automatic update then. This can be overirdden in user
# settings.

# filenames will not contain spaces. Thus keeping the for-loop simple for
# better maintainability
# shellcheck disable=SC2044

# don't run ssid_changer, if the wizard wasn't run yet.
if [ ! -f /etc/config/ffwizard ]; then
    log "ffwizard didn't run yet. Cancelling scripts run."
    exit 1
fi

watch_dirs="/etc/config/"
chksum_dir="/etc/autoupdate/cheksums"

for dir in $watch_dirs; do
    mkdir -p "$chksum_dir""$dir"
    # filename will not contain spaces.
    for file in $(find "$dir" -type f); do
        md5=$(md5sum "$file" | cut -d' ' -f 1)
        echo "$md5" > "$chksum_dir""$file"
    done
done
