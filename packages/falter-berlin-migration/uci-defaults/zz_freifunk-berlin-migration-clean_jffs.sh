#!/usr/bin/env sh

# delete any overlay config files duplicated from romfs by sysupgrade - saves JFFS2 space
cd /overlay/upper/etc/config/ && for i in *; do [ -f "$i" ] && if cmp -s "$i" "/rom/etc/config/$i"; then rm -f "$i"; fi; done;
