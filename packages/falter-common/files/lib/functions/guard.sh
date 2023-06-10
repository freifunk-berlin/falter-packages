#!/bin/sh

CFG="defaults"
CFG_FILE="/etc/config/$CFG"
SECTION="packages"
NAME="default"

# check if defaults have already been set
guard() {
  OPTION=$1

  [ ! -f $CFG_FILE ] && touch $CFG_FILE && uci set $CFG.$NAME=$SECTION
  [ "$(uci -q get "$CFG.$NAME.$OPTION")" = "1" ] && exit 0
  uci set "$CFG.$NAME.$OPTION=1"
  uci commit $CFG
}

guard_rename() {
  SRC=$1
  DEST=$2

  # get current setting or exit when not defined
  cur_set=$(uci -q get "$CFG.$NAME.$SRC") || return 0
  uci delete "$CFG.$NAME.$SRC"
  uci set "$CFG.$NAME.$DEST=$cur_set"
  uci commit $CFG
}

guard_delete() {
  OPTION=$1

  uci -q delete "$CFG.$NAME.$OPTION" || return 0
  uci commit $CFG
}
