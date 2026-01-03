#!/bin/sh

# This software originates from Freifunk Berlin and implements a basic autoupdate mechanism
# by using OpenWrts built-in sysupgrade.
# It is licensed under GNU General Public License v3.0 or later
# Copyright (C) 2022   Martin Hübner and Tobias Schwarz

# shellcheck shell=dash

# jshn assigns the variables for us, but shellcheck doesn't get it.
# shellcheck disable=SC2154
# We don't need the return values and check the correct execution in other ways.
# shellcheck disable=SC2155
# using printf with variables and nc didn't work correctly. Thus this hack
# shellcheck disable=SC2059
# FW_SERVER_URL isn't mispelled, but a global variable defined in autoupdate.sh
# shellcheck disable=SC2153

# Those dependencies aren't available for CI checking.
# shellcheck source=/dev/null
. /lib/functions.sh
. /lib/functions/semver.sh
. /lib/config/uci.sh
. /usr/share/libubox/jshn.sh

# except than noted, this script is not posix-compliant in one way: we use "local"
# variables definition. As nearly all shells out there implement local, this should
# work anyway. This is a little reminder to you, if you use some rare shell without
# a builtin "local" statement.

set -o pipefail

log() {
    local msg="$1"
    logger -t autoupdater -s "$msg"
}

load_overview_and_certs() {
    # we assume, that the autoupdate.json was signed by different developers. The
    # files are named in this order:
    # autoupdate.json.1.sig
    # autoupdate.json.2.sig
    # autoupdate.json.3.sig and so forth

    local fw_url="$1"

    # load autoupdate.json
    wget -q "$fw_url" -O "$PATH_DIR/autoupdate.json"
    ret_code=$?
    if [ $ret_code != 0 ]; then
        return $ret_code
    fi

    # load certificates
    local cnt=1
    while wget -q "$fwurl.$cnt.sig" -O "$PATH_DIR/autoupdate.json.$cnt.sig"; do
        cnt=$((cnt + 1))
    done
}

read_latest_stable() {
    # reads the latest firmware version from the autoupdate.json

    local path_autoupdate_json="$1"

    cat "$path_autoupdate_json" | grep -F 'falter-version' | sed -e 's|.*"falter-version":\s*"\([^"]*\)".*|\1|g'

    return $?
}

get_firmware_flavour() {
    # echos the freifunk-berlin firmware flavour like
    # tunneldigger, notunnel, ...

    local flavour="$(uci_get ffberlin-uplink preset current)"

    if [ "$flavour" = "tunnelberlin_tunneldigger" ]; then
        echo "tunneldigger"
    elif [ "$flavour" = "notunnel" ]; then
        echo "notunnel"
    else
        echo "unknown"
    fi
}

get_board_target() {
    # echo the boards target. i.e. ath79/generic

    local target=""

    json_init
    json_load "$(ubus call system board)"
    json_select release
    json_get_var target target
    json_cleanup

    echo "$target"
}

iter_images() {
    # iterates over the images available for a board
    # and finds the file name of the sysupgrade-image.
    # Sets global vars (named in CAPS)

    json_select "$2"
    json_get_var "image_type" "type"

    if [ "$image_type" = "sysupgrade" ]; then
        json_get_var image_name name
        IMAGE_NAME="$image_name"
        json_get_var image_hash sha256
        IMAGE_HASH="$image_hash"
        # don't take image hash from unsigned file, but from signed autoupdate.json
    fi

    json_select ..
}

request_file_size() {
    # fetches HTTP-Header of given file and returns its size in KiB
    local url="$1"
    local fqdn="$(echo "$url" | cut -d'/' -f3)"
    local file="/$(echo "$url" | cut -d'/' -f4-)"

    size_bytes=$(printf "GET $file HTTP/1.0\r\nHost: $fqdn\r\nConnection: close\r\n\r\n" | nc "$fqdn" 80 | head | grep "Content-Length" | cut -d':' -f2)

    return $((size_bytes / 1024))
}

get_download_link_and_hash() {
    # echos the download-Link and the sha256_sum it should have in a string of format:
    #       "$DOWLOAD_LINK $HASH_SUM"
    #
    # capitalised vars get modified by called functions directly

    local _version="$1"
    local flavour="$2"

    local board="$(board_name)"
    local target=""
    local profile=""
    local profiles_url=""
    local hash_expected=""
    local image_name=""
    local image_hash=""

    json_cleanup
    json_init
    json_load_file "$PATH_DIR/autoupdate.json"
    json_select devices
    json_select "$board"
    json_get_var profile profile
    json_get_var target target
    json_select ..
    json_select ..
    json_select profiles
    json_select "$target"
    json_select "$flavour"
    json_get_var profiles_url url
    json_get_var hash_expected sha256sum
    json_cleanup

    rm -f "$PATH_DIR/profiles.json"
    wget -O "$PATH_DIR/profiles.json" "https://${FW_SERVER_URL}$profiles_url"
    local hash_actual="$(sha256sum "$PATH_DIR/profiles.json" | cut -d' ' -f1)"
    if [ "$hash_actual" != "$hash_expected" ]; then
        log "failed to verify profiles.json - expected=$hash_expected actual=$hash_actual"
        exit 2
    fi

    json_init
    json_load_file "$PATH_DIR/profiles.json"
    json_select profiles
    json_select "$profile"
    json_for_each_item iter_images images
    json_cleanup

    if [ -z "$IMAGE_NAME" ]; then
        log "Failed to get image download link. There might be no automatic update for your Router. This can have several reasons. You may try to find a newer frimware by yourself."
        exit 2
    fi

    echo "https://${FW_SERVER_URL}$(dirname "$profiles_url")/$IMAGE_NAME $IMAGE_HASH"
}

verify_image_hash() {
    # verifies an image to have a certain hash-sum
    # returns 0 if they match, 1 else

    local image_path="$1"
    local dest_hash_sum="$2"
    local curr_hash_sum=""

    curr_hash_sum=$(sha256sum "$image_path" | cut -d' ' -f 1)

    if [ "$curr_hash_sum" = "$dest_hash_sum" ]; then
        return 0
    else
        return 1
    fi
}

pop_element() {
    # give the list to be worked on via list_old. Set it to the list, before invoking the function.
    # search for $2 and pop it from list. After that, print new list.

    local list_old="$1"
    local string="$2"
    local list_new=""
    local element=""

    #add every element, which doesn't match the string to new list
    for element in $list_old; do
        if [ "$string" = "$element" ]; then
            continue
        else
            list_new="$list_new ""$element "
        fi
    done

    echo "$list_new"
}

min_valid_certificates() {
    # for every certificate, iterate over known public keys and
    # count matches. Assure that we don't validate against a key two times
    # by removing it from list, once it was used.
    # returns true, if we got minimum certificates, false if not.

    local signed_file="$1"
    local min_cnt="$2"
    local cert_list=""
    local key_list=""
    local cert_cnt=0

    cert_list=$(find "$PATH_DIR/" -name "*.sig")
    key_list=$(find "$KEY_DIR" -name "*.pub")

    for cert in $cert_list; do
        for key in $key_list; do
            if usign -V -p "$key" -m "$signed_file" -x "$cert" 2>/dev/null; then
                cert_cnt=$((cert_cnt + 1))
                #pop key from list. Thus one key cannot validate multiple certs.
                key_list=$(pop_element "$key_list" "$key")
            fi
            if [ $cert_cnt = "$min_cnt" ]; then
                return 255
            fi
        done
    done

    return $cert_cnt
}
