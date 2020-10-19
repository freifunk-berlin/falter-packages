local uci = require "luci.model.uci".cursor()
local sys = require "luci.sys"
local tools = require "luci.tools.freifunk.assistent.ffwizard"
local ip = require "luci.ip"
local ipkg = require "luci.model.ipkg"
local fs = require "nixio.fs"

local olsr = require "luci.tools.freifunk.assistent.olsr"
local firewall = require "luci.tools.freifunk.assistent.firewall"
local fftools = require "luci.tools.freifunk.assistent.tools"

module ("luci.controller.assistent.assistent", package.seeall)

function index()
  if not nixio.fs.access("/usr/lib/lua/luci/controller/freifunk/freifunk.lua") then
    entry({"admin", "freifunk"}, firstchild(), "Freifunk", 5).dependent=false
  end
  entry({"admin", "freifunk", "assistent"}, call("prepare"), "Freifunkassistent", 1).dependent=false
  entry({"admin", "freifunk", "assistent", "changePassword"}, form("freifunk/assistent/changePassword"), nil,1)
  entry({"admin", "freifunk", "assistent", "generalInfo"}, form("freifunk/assistent/generalInfo"), nil, 1)
  entry({"admin", "freifunk", "assistent", "decide"}, template("freifunk/assistent/decide"), nil, 2)
  entry({"admin", "freifunk", "assistent", "sharedInternet"}, form("freifunk/assistent/shareInternet"), nil, 10)
  entry({"admin", "freifunk", "assistent", "wireless"}, form("freifunk/assistent/wireless"), nil, 20)
  entry({"admin", "freifunk", "assistent", "optionalConfigs"}, form("freifunk/assistent/optionalConfigs"), nil, 20)
  entry({"admin", "freifunk", "assistent", "applyChanges"}, call("commit"), nil, 100)
  entry({"admin", "freifunk", "assistent", "reboot"}, template("freifunk/assistent/reboot"), nil, 101)
  entry({"admin", "freifunk", "assistent", "cancel"}, call("reset"), nil, 102)
end

function prepare()
  if not fs.access("/etc/config/ffwizard") then
    fs.writefile("/etc/config/ffwizard", "")
    uci:set("ffwizard", "settings", "settings")
    uci:save("ffwizard")
    uci:commit("ffwizard")
  end
  if not uci:get("ffwizard","settings","runbefore") and not fftools.hasRootPass() then
    luci.http.redirect(luci.dispatcher.build_url("admin/freifunk/assistent/changePassword"))
  else
    luci.http.redirect(luci.dispatcher.build_url("admin/freifunk/assistent/generalInfo"))
  end
end

function commit()

  uci:set("ffwizard","settings","runbefore","true")
  uci:save("ffwizard")
  local sharenet = uci:get("ffwizard","settings","sharenet")

  local community = "profile_"..uci:get("freifunk","community","name")

  --change hostname to mesh ip if it is still the default (Openwrt-something, LEDE, e.g.)
  if string.sub(uci:get_first("system","system","hostname"),1,string.len("gib-mir-einen-namen"))=="gib-mir-einen-namen" then
    local dhcpmesh = uci:get("ffwizard","settings","dhcpmesh")
    dhcpmesh = ip.IPv4(dhcpmesh):minhost()
    uci:foreach("system", "system",
      function(section)
        local newhostname = dhcpmesh:string():gsub("%.", "-")
        uci:set("system",section[".name"],"hostname", newhostname)
      end)
  end

  --remove geo data if it is still default
  local latval = tonumber(uci:get_first("system","system","latitude"))
  local lonval = tonumber(uci:get_first("system","system","longitude"))
  local latval_com = tonumber(uci:get_first(community,"community","latitude"))
  local lonval_com = tonumber(uci:get_first(community,"community","longitude"))

  if latval and latval == 52 then
    latval = nil
  end
  if latval and latval == latval_com then
    latval = nil
  end
  if lonval and lonval == 13 then
    lonval = nil
  end
  if lonval and lonval == lonval_com then
    --this is always false?: o_0
    lonval = nil
  end
  if not lonval or not latval then
    uci:foreach("system","system",
      function(s)
        uci:delete("system", s[".name"], "latitude")
        uci:delete("system", s[".name"], "longitude")
      end)
    uci:save("system")
  end

  firewall.configureFirewall()

  olsr.configureOLSR()
  olsr.configureOLSRPlugins()

  tools.configureQOS()

  uci:commit("dhcp")
  uci:commit("olsrd")
  uci:commit("olsrd6")
  uci:commit("firewall")
  uci:commit("system")
  uci:commit("ffwizard")
  uci:commit("freifunk")
  uci:commit("wireless")
  uci:commit("network")
  uci:commit("qos")

  sys.init.enable("olsrd")
  sys.init.enable("olsrd6")
  -- openvpn gets started by wan hotplug script
  sys.init.disable("openvpn")

  if (sharenet == "1") then
    sys.init.enable("qos")
  end

  if ipkg.installed("luci-app-statistics") == true then
    local enableStats = uci:get("ffwizard", "settings", "enableStats") or "0"
    uci:foreach("luci_statistics", "statistics",
      function(s)
        if (s['.name'] ~= 'collectd' and s['.name'] ~= 'rrdtool') then
          uci:set("luci_statistics", s['.name'], "enable", enableStats)
        end
      end)
    uci:save("luci_statistics")
    uci:commit("luci_statistics")
  end

  sys.hostname(uci:get_first("system","system","hostname"))

  luci.http.redirect(luci.dispatcher.build_url("admin/freifunk/assistent/reboot"))
end

function reset()
  uci:revert("dhcp")
  uci:revert("olsrd")
  uci:revert("olsrd6")
  uci:revert("firewall")
  uci:revert("system")
  uci:revert("freifunk")
  uci:revert("wireless")
  uci:revert("network")
  uci:revert("qos")

  if ipkg.installed("luci-app-statistics") == True then
    uci:revert("luci-app-statistics")
  end

  uci:set("ffwizard","settings","runbefore","true")
  uci:save("ffwizard")
  uci:commit("ffwizard")

  luci.http.redirect(luci.dispatcher.build_url("/"))
end
