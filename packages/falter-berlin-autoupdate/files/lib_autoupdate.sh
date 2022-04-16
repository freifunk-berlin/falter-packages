#!/bin/sh

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

get_latest_stable() {
    # loads the configuration-file of the firmware-selector
    # and scans for the firmware selected by default (-> latest stable)

    local selector_url="$1"

    wget -qO - "https://${selector_url}/config.js" | grep default_version | sed -e 's|.*\([0-9].[0-9].[0-9]\).*|\1|'
    return $?
}

load_overview_and_certs() {
    # we assume, that the autoupdate.json was signed by different developers. The
    # files are named in this order:
    # autoupdate.json.1.sig
    # autoupdate.json.2.sig
    # autoupdate.json.3.sig and so forth

    local fw_server_url="$1"
    local fw_version="$2"

    # load autoupdate.json
    wget -q "https://${fw_server_url}/stable/${fw_version}/autoupdate.json" -O "$PATH_DIR/autoupdate.json"
    ret_code=$?
    if [ $ret_code != 0 ]; then
        return $ret_code
    fi

    # load certificates
    local cnt=1
    while wget -q "https://${fw_server_url}/stable/${fw_version}/autoupdate.json.$cnt.sig" -O "$PATH_DIR/autoupdate.json.$cnt.sig"; do
        cnt=$((cnt + 1))
    done
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

get_board_name() {
    # echos the boards name like used in firmware-selector
    board_name | tr ',' '_'
}

get_board_target() {
    # echo the boards target. i.e. ath79/generic

    local target=""

    json_init
    json_load "$(ubus call system board)"
    json_select release
    json_get_var target target

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
        #json_get_var image_hash sha256
        IMAGE_NAME="$image_name"
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

    local version="$1"
    local flavour="$2"
    local autoupdate_json=""
    local curr_target=""
    local NEW_TARGET=""
    local BOARD=""
    local board_json=""
    local IMAGE_NAME=""
    local image_hash=""

    load_overview_and_certs "$FW_SERVER_URL" "$version"
    autoupdate_json=$(cat "$PATH_DIR/autoupdate.json")

    BOARD=$(get_board_name)

    # extract download-link from json-string
    # whith jshn it takes ages to parse that big json-file. As we only need the image-base-url, scrape it with sed
    base_url=$(echo "$autoupdate_json" | grep image_url | sed -e 's|.*\(http.*{target}\).*|\1|g')

    # Idea: Don't check for target-change. If the Target changed, the download will fail anyway.
    curr_target=$(get_board_target)

    # get secure hash from signed autoupdate.json
    json_init
    json_load_file "$PATH_DIR/autoupdate.json"
    json_select target
    json_select "$curr_target"
    json_select "$BOARD"
    json_get_var image_hash "$flavour"

    # load board-specific json with download-links from selector
    board_json=$(wget -qO - "https://${SELECTOR_URL}/${version}/${flavour}/${curr_target}/${BOARD}.json")

    json_init
    json_load "$board_json"
    json_for_each_item "iter_images" "images"

    if [ -z "$IMAGE_NAME" ]; then
        log "Failed to get image download link. There might be no automatic update for your Router. This can have several reasons. You may try to find newer frimware by yourself."
        exit 2
    fi

    # construct download-link
    echo "$base_url" | sed -e "s|{falter-version}|$version|g;s|{flavour}|$flavour|g;s|{target}|$curr_target/$IMAGE_NAME $image_hash|g"
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

    cert_list=$(find "$PATH_DIR/" -name *.sig)
    key_list=$(find "$KEY_DIR" -name *.pub)

    for cert in $cert_list; do
        for key in $key_list; do
            if usign -V -p "$key" -m "$signed_file" -x "$cert" 2>/dev/null; then
                cert_cnt=$((cert_cnt + 1))
                #pop key from list. Thus one key cannot validate multiple certs.
                key_list=$(pop_element "$key_list" "$key")
            fi
            if [ $cert_cnt = $min_cnt ]; then
                return 255
            fi
        done
    done

    return $cert_cnt
}