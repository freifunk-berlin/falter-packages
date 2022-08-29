#!/bin/sh

# register all keys in directory keys/ into the dropbears key-file

SCRIPTPATH=$(dirname "$(readlink -f "$0")")
KEY_FILES=$(find "$SCRIPTPATH" -name "*.pub")

for KEY_FILE in $KEY_FILES; do
    KEY=$(cat "$KEY_FILE")
    if ! grep -q "$KEY" /etc/dropbear/authorized_keys; then
        echo "$KEY" >> /etc/dropbear/authorized_keys
    fi
done
