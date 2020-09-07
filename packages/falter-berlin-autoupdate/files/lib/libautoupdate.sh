#internal definitions for autoupdate

PATH_PUB_KEY="/usr/share/autoupdate/keys/"
PATH_BIN="/tmp/sysupgrade.bin"
PATH_TMP="/tmp/autoupdate/"
PATH_JSON="/tmp/router.json"
PATH_BAK="/tmp/backup.tar.gz"
PATH_AUTOBAK="/root/backup/"

#load json-functions
. /usr/share/libubox/jshn.sh

#funtions for autoupdate

get_date() {
    TODAY=$(date -u +%Y-%m-%d)
}

get_hostname() {
    HOSTNAME=$(uci -q get system.@system[0].hostname)
}

get_branch() {
    JSON_LINK="$JSON_LINK_SERVER""$BRANCH"".json"
}

create_tmp() {
    if [ ! -d "$PATH_TMP" ]; then
        mkdir "$PATH_TMP"
    fi
}

#download the link definition file and all its signatures.
get_def() {
    create_tmp
    # variable used by several other functions.
    PATH_JSON="$PATH_TMP""$BRANCH"".json"

    #get a list of link-def and all its signatures. Download them.
    local FILE
    local FILES
    FILES=$(wget -q "$JSON_LINK_SERVER" -O - | cut -d'"' -f 8 | grep "$BRANCH")

    for FILE in $FILES; do
        wget "$JSON_LINK_SERVER""$FILE" -P "$PATH_TMP" 2>/dev/null
    done

    #check if download was successful
    if [ ! -f "$PATH_JSON" ]; then
        echo "Download of link definition file failed."
        logger -t "autoupdate" "Download of link definition file failed."
        exit 1
    fi
}

pop_element() {
    #give the list to be worked on via global var POP_LIST. Set it to the list, before invoking the function.
    #search for $1 and pop it from list. After that, print new list.
    local LIST_OLD
    local LIST_NEW
    local ELEMENT
    local STRING
    STRING="$1"

    #add every elemt, which doesn't match the string to new list
    for ELEMENT in $POP_LIST; do
        if [ "$STRING" = "$ELEMENT" ]; then
            continue
        else
            LIST_NEW="$LIST_NEW""$ELEMENT "
        fi
    done

    #print new list
    echo $LIST_NEW
}

#check the links against a public key, to verify their origin
verify_def() {
    local CERT_COUNTER
    CERT_COUNTER=0
    #MIN_CERTS = minimum amount of valid certificates needed to proceed. Was loaded in files/autoupdate at the script start
    #get paths of public keys
    KEYS=$(find $PATH_PUB_KEY -name '*.pub')
    #get paths of all signature-files in /tmp/auotupdate/
    CERTS=$(find "$PATH_TMP" -name "*.sig")

    for CERT in $CERTS; do
        for KEY in $KEYS; do
            usign -V -p $KEY -m "$PATH_JSON" -x $CERT 2>/dev/null
            if [ $? = 0 ]; then
                CERT_COUNTER=$(($CERT_COUNTER + 1))
                #pop key from list. Thus key cannot validate multiple certs.
                POP_LIST="$KEYS"
                KEYS=$(pop_element $KEY)
            fi
        done
    done

    if [ $CERT_COUNTER -ge $MIN_CERTS ]; then
        logger -t "autoupdate" "Link definition file was signed with $CERT_COUNTER valid Certs."
        echo "Link definition file was signed with $CERT_COUNTER valid Certs."
        echo "Verification successful."
        return
    fi

    logger -t "autoupdate" "Verification failed. File was signed by $CERT_COUNTER valid Certs only."
    echo ""
    echo "ERROR: Verification failed. File was signed by $CERT_COUNTER valid Certs only."
    echo ""
    exit 1
}

#get the router model string. CAUTION: sometimes this string does not match the hardware
get_router() {
    if [ "$ROUTER" = "auto" ]; then
        ROUTER=$(grep machine /proc/cpuinfo | cut -d':' -f 2 | cut -c 2-)
    fi
    if [ -z "$ROUTER" ]; then
        echo "Couldn't get the router-model. Please refer to the manual how to define it."
        logger -t "autoupdate" "Couldn't get router-model. Refer the manual, please."
        exit 1
    fi
}

commit_routerstring() {
    get_router
    ROUTER2=$(echo "$ROUTER" | sed -f /usr/share/autoupdate/lib/urlencode.sed)
    wget "$DOMAIN/devicename;$ROUTER2;" 2>/dev/null
    #we 'send' the string into the webservers log and can grab them from there via a script. Server will normally give HTTP error 404.
    if [ $? == 8 ]; then
        echo "String submitted successfully."
    else
        echo "An error occured while submitting the string. Is there a way to the internet?"
    fi
}

#determine the current firmware-type (tunnel/no-tunnel, etc)
get_type() {
    if [ "$TYPE" = "auto" ]; then
        UPLINK=$(uci -q get ffberlin-uplink.preset.current)
        if [ "$UPLINK" = "tunnelberlin_tunneldigger" ]; then
            TYPE="tunneldigger"
        else
            TYPE="default"
        fi
    fi
}

create_backup_file() {
    sysupgrade -b "$PATH_BAK"
}

create_backup_dir() {
    if [ ! -d "$PATH_AUTOBAK" ]; then
        mkdir "$PATH_AUTOBAK"
    fi
}

empty_backup_dir() {
    local WC
    WC=$(ls "$PATH_AUTOBAK" | wc -l)
    if [ $WC != 0 ]; then
        rm "$PATH_AUTOBAK"*
    fi
}

set_preserved_backup_dir() {
    local CONF
    CONF="/etc/sysupgrade.conf"
    #set $PATH_AUTOBAK to be preserved on reflash
    grep -q "$PATH_AUTOBAK" "$CONF"
    if [ $? = 1 ]; then
        echo "$PATH_AUTOBAK" >>"$CONF"
    fi
}

remote_backup() {
    if [ "$CLIENT_NAME" = "complete-hostname" ] && [ "$CLIENT_USER" = "user" ] && [ "$CLIENT_PATH" = "/complete/path/to/your/backups/" ]; then
        echo ""
        echo "You must specify some settings for this feature."
        echo "Have a look at: /etc/config/autoupdate"
        echo ""
        exit 1
    fi
    get_date
    get_hostname
    create_backup_file
    scp "$PATH_BAK" "$CLIENT_USER""@$CLIENT_NAME"":$CLIENT_PATH""$TODAY""_$HOSTNAME"".tar.gz"
}

#create backup archive and save it in /root/backup/.
do_auto_backup() {
    create_backup_dir
    empty_backup_dir
    set_preserved_backup_dir
    get_date
    get_hostname
    create_backup_file
    #copy backupdata to save memory
    cp "$PATH_BAK" "$PATH_AUTOBAK""$TODAY""_$HOSTNAME"".tar.gz"

    #set $PATH_AUTOBAK to be preserved on reflash
    grep -q "$PATH_AUTOBAK" /etc/sysupgrade.conf
    if [ $? = 1 ]; then
        echo "$PATH_AUTOBAK" >>/etc/sysupgrade.conf
    fi
}

chk_upgr() {
    #check if there happened any changes in the link-file
    JSON=$(cat "$PATH_JSON")
    json_load "$JSON"
    json_get_var JDATE creation_time
    if [ $JDATE -lt $LAST_UPGR ]; then
        logger -t "autoupgrade" "Updatecheck: No updates avaiable."
        echo "No updates avaiable."
        exit 0
    fi
}

#get link to the new firmware file
get_link() {
    JSON=$(cat $PATH_JSON)
    json_load "$JSON"
    json_get_var JDATE creation_time
    json_select "$ROUTER"
    json_get_var LINK $TYPE
    if [ -z "$LINK" ]; then
        logger -t "autoupdate" "No upgrade done. This router is not supported by autoupgrade."
        exit 0
    fi
}

#download new firmware binary.
get_bin() {
    wget $LINK -O $PATH_BIN
}

write_update_date() {
    TODAY=$(date -u +%Y%m%d)
    uci set autoupdate.internal.last_upgr="$TODAY"
    uci commit autoupdate
}

get_package_list() {
    get_date
    local LIST
    LIST=$(opkg list-installed | cut -d ' ' -f 1)
    echo $LIST >>"$PATH_AUTOBAK""/$TODAY""_package-list"
}
