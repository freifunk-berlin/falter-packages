#! /bin/sh

set -e

. /usr/share/libubox/jshn.sh

TMP_FILE="/tmp/tmp.json"
SOURCE_FILE="/usr/share/luci/menu.d/luci-base.json"
# delete "methods": [ "cookie:sysauth" ]
sed -En '/"admin\/menu"/q;p' $SOURCE_FILE > $TMP_FILE && sed -En '/"admin\/menu"/,/END/p' $SOURCE_FILE | sed -En '/\W*"methods"/!p' >> $TMP_FILE
cp $TMP_FILE $SOURCE_FILE
#json_init
#json_load_file /usr/share/luci/menu.d/luci-base.json
#json_select "admin/menu"
#json_add_object "auth"
#json_close_object
#json_dump -i > /usr/share/luci/menu.d/luci-base.json

# replace html-stuff in /usr/lib/lua/luci/view/themes/bootstrap/footer.htm
sed -ni '1h;1!H;${;g;s/<% if luci.dispatcher.context.authsession then %>.*\n\W*<script type="text\/javascript">L.require(.menu-bootstrap.)<.script>\n\W*<% end %>/<script type="text\/javascript">L\.require('"'menu-bootstrap'"')<\/script>/g;p;}' /usr/lib/lua/luci/view/themes/bootstrap/footer.htm

exit 0
