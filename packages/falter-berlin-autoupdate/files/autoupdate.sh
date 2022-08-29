#!/bin/sh

# This software originates from Freifunk Berlin and implements a basic autoupdate mechanism
# by using OpenWrts built-in sysupgrade.
# It is licensed under GNU General Public License v3.0 or later
# Copyright (C) 2022   Martin Hübner and Tobias Schwarz

# shellcheck shell=dash

# except than noted, this script is not posix-compliant in one way: we use "local"
# variables definition. As nearly all shells out there implement local, this should
# work anyway. This is a little reminder to you, if you use some rare shell without
# a builtin "local" statement.

# We don't need the return values and check the correct execution in other ways.
# shellcheck disable=SC2155

# we can't check those dependencies at the CI
# shellcheck source=/dev/null
. /lib/functions.sh
. /lib/config/uci.sh
. /etc/freifunk_release

cleanup() {
    log "exiting..."

    if [ -d "$PATH_DIR" ] && [ -z "$OPT_TESTRUN" ]; then
        rm -rf "$PATH_DIR"
    fi

    exit
}
trap cleanup EXIT

print_help() {
    printf "\

Autoupdate: Tool for updating Freifunk-Berlin-Firmware automatically

Mostly you should call this programm without any options. If you specify
options on the command line, they will superseed the options from the
configuration file.

Optional arguments:
    -h: show this help text
    -i: ignore certs
            Don't prove the images origin by checking the certificates
    -m INT: minimum certs
            flash image, if it was signed by minimum amount of certs
    -n: new installation
            flash the image and wipe configuration. So you will start
            with a new wizard-run.
    -t: test-run
            this will perform everything like in automatic-mode, except
            that it won't flash the image and won't tidy up afterwards.
    -f: force update
            CAUTION: This will ignore all checks except the certificates
            and checksums!

Example call:
    autoupdate
\n"
}

##########################
#   Load Configuration   #
##########################

export SELECTOR_URL=$(uci_get autoupdate cfg selector_fqdn)
FW_SERVER_URL=$(uci_get autoupdate cfg fw_server_fqdn)
MIN_CERTS=$(uci_get autoupdate cfg minimum_certs)
DISABLED=$(uci_get autoupdate cfg disabled)

PATH_DIR="/tmp/autoupdate"
PATH_BIN="$PATH_DIR/freifunk_syupgrade.bin"
export KEY_DIR="/etc/autoupdate/keys/"

MIN_RAM_FREE=1536 # amount of kiB that must be free in RAM after firmware-download

# load lib-autoupdate after configuration-load, to substitute global vars...
. /lib/autoupdate/lib_autoupdate.sh

#####################
#   Main Programm   #
#####################

#########################
#  Commandline parsing

while getopts him:ntf option; do
    case $option in
    h)
        print_help
        exit 0
        ;;
    i) OPT_IGNORE_CERTS=1 ;;
    m) MIN_CERTS=$OPTARG ;;
    n) OPT_N=1 ;;
    t) OPT_TESTRUN=1 ;;
    f) OPT_FORCE=1 ;;
    *)
        printf "\nUnknown argument! Please use valid arguments only.\n\n"
        print_help
        exit 2
        ;;
    esac
done

# sanitise min-certs-input
MIN_CERTS=$(echo "$MIN_CERTS" | sed -e 's|[^0-9]||g')
if [ -z "$MIN_CERTS" ]; then
    echo "please give numbers only for -m"
    exit 2
fi

log "starting autoupdate..."

#############################
#  Checks and Checks again

is_stable_release=$(echo "$FREIFUNK_RELEASE" | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$')
if [ -z $OPT_FORCE ] && [ -z "$is_stable_release" ]; then
    log "automatic updates aren't supported for development-firmwares. Please update manually or use the force update option '-f' or new-install option '-n'."
    exit 2
fi

if [ -z $OPT_FORCE ] && { [ "$DISABLED" = "1" ] || [ "$DISABLED" = "yes" ] || [ "$DISABLED" = "true" ]; }; then
    log "autoupdate is disabled. Change the configs at /et/config/autoupdate to enable it."
    exit 2
fi

UPTIME=$(cut -d'.' -f1 </proc/uptime)
# only update, if router runs for at least two hours (so the update probably won't get disrupted)
if [ -z $OPT_FORCE ] && [ "$UPTIME" -lt 7200 ]; then
    log "Router didn't run for two hours. It might be just plugged in for testing. Aborting..."
    exit 2
fi

# create tmp-dir
rm -rf "$PATH_DIR"
mkdir -p "$PATH_DIR"

log "fetch autoupdate.json from $FW_SERVER_URL ..."
if load_overview_and_certs "$FW_SERVER_URL"; then
    log "fetching autoupdate.json failed. Probably no internet connection."
    exit 2
fi
log "done."

# prove to be signed by minimum amount of certs
if [ -z $OPT_IGNORE_CERTS ]; then
    log "Verifying image-signatures..."
    min_valid_certificates "$PATH_DIR/autoupdate.json" "$MIN_CERTS"
    ret_code=$?
    if [ $ret_code != 255 ]; then
        log "autoupdate.json was signed by $ret_code certificates only. At least $MIN_CERTS required."
        exit 2
    else
        log "autoupdate.json was signed by at least $MIN_CERTS certificates. Continuing..."
    fi
else
    log "ignoring certificates as requested."
fi

if latest_release=$(read_latest_stable "$PATH_DIR/autoupdate.json"); then
    log "wasn't able to read latest stable version from autoupdate.json"
    exit 2
else
    log "latest release is $latest_release"
fi

##################
#  Update-stuff

if semverLT "$FREIFUNK_RELEASE" "$latest_release"; then

    router_board=$(get_board_name)
    flavour=$(get_firmware_flavour)
    log "router board is: $router_board. firmware-flavour is: $flavour."
    if [ "$flavour" = "unknown" ]; then
        log "failed to determine the firmware-type of your installation. Please consider a manual update."
        exit 1
    fi

    log "parsing download-link and images hashsum (takes up to 40 seconds)..."
    link_and_hash=$(get_download_link_and_hash "$latest_release" "$flavour")
    log "done."

    link=$(echo "$link_and_hash" | cut -d' ' -f 1)
    hash_sum=$(echo "$link_and_hash" | cut -d' ' -f 2)

    log "download link is: $link."

    # delete json and signatures to save space in RAM
    if [ -z $OPT_TESTRUN ]; then
        json_sig_files=$(find "$PATH_DIR" -name "autoupdate.json*")
        for f in $json_sig_files; do
            rm "$f"
        done
    fi

    log "Try loading new firmware..."

    # check if the firmware-bin would fit into tmpfs
    request_file_size "$link"
    size=$?
    freemem=$(free | grep Mem | sed -e 's| \+| |g' | cut -d' ' -f 4)
    # only load the firmware, if there would be 1.5 MiB left in RAM
    if [ $((freemem - MIN_RAM_FREE)) -lt $size ]; then
        log "there is not enough ram on your device to download the image. You might free some memory by stopping some services before the update."
        exit 2
    fi

    # download image to /tmp/autoupdate
    wget -qO "$PATH_BIN" "$link"
    ret_code=$?
    if [ $ret_code != 0 ]; then
        log "failed! wget returned $ret_code."
        exit 2
    else
        log "done."
    fi

    # verify image to be correct
    verify_image_hash "$PATH_BIN" "$hash_sum"
    ret_code=$?
    if [ $ret_code != 0 ]; then
        log "The expected hash of the loaded image didn't match the real one."
        exit 2
    else
        log "Image hash is correct. sha256sum: $hash_sum"
    fi

    # flash image
    if [ -z $OPT_TESTRUN ]; then
        log "start flashing the image..."
        if [ -n "$OPT_N" ]; then
            sysupgrade -n "$PATH_BIN"
        else
            sysupgrade "$PATH_BIN"
        fi
        log "done."
    fi
else
    log "v$FREIFUNK_RELEASE is the latest version. Nothing to do. I will recheck tomorrow."
fi
