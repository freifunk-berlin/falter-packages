map= Map("autoupdate", "autoupdate") -- We want to edit the uci config file /etc/config/autoupdate

sec_auto = map:section(NamedSection, "automode", "settings", "Automatische Updates")
c = sec_auto:option(ListValue, "automode", "automode", "Updates selbstständig durchführen?")
c:value("true", "ja") -- key and display-value pairs
c:value("false", "nein")
c.default = "false"
d = sec_auto:option(Value, "branch", "Branch")
d:value("release")
d:value("testing")
d:value("development")
d.default = "release"


sec_router = map:section(NamedSection, "router", "settings", "Router", "Hier sollte nur etwas geändert werden, wenn automatische Updates nicht funktionieren. Der Standardwert ist 'auto' für automatische Erkennung.")
a = sec_router:option(Value, "model", "Routermodell", "z.B.: TP-LINK TL-WR1043ND v2")
a:value("auto", "automatisch erkennen")
a.default = "auto"
b = sec_router:option(ListValue, "type", "Uplink-Type")
b:value("default", "default - direkt ausleiten")
b:value("tunneldigger", "tunneldigger - über VPN ausleiten")
b:value("auto", "automatisch erkennen")
b.default = "auto"

sec_internal = map:section(NamedSection, "internal", "settings", "Interne Einstellungen")
d = sec_internal:option(Value, "json_link_server", "Link-Listen", "URL, unter der nach Link-Listen gesucht wird.")
d.default = "null"
e = sec_internal:option(Value, "minimum_signatures", "Anzahl Signaturen", "Mindestanzahl gültiger Signaturen, um ein Update durchführen zu können.")
e.default = "4"

return map -- Returns the map
