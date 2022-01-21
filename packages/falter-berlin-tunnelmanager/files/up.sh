#!/bin/sh

interface="$1"

# Configure OLSRD
uci revert olsrd

UCIREF="$(uci add olsrd Interface)"
uci set "olsrd.$UCIREF.ignore='0'"
uci set "olsrd.$UCIREF.interface='$interface'"
uci set "olsrd.$UCIREF.Mode='ether'"
uci commit olsrd

/etc/init.d/olsrd reload


# Configure babeld
uci revert babeld
UCIREF="$(uci add babeld interface)"
uci set "babeld.$UCIREF.ignore='0'"
uci set "babeld.$UCIREF.interface='$interface'"
uci set "babeld.$UCIREF.Mode='ether'"
uci commit babeld

/etc/init.d/babeld reload
