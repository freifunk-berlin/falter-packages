local uci = require "luci.model.uci".cursor()
local ip = require "luci.ip"
local util = require "luci.util"
local tools = require "luci.tools.freifunk.assistent.tools"
local ipkg = require "luci.model.ipkg"

local olsr = require "luci.tools.freifunk.assistent.olsr"
local firewall = require "luci.tools.freifunk.assistent.firewall"
olsr.prepareOLSR()
firewall.prepareFirewall()

local device_l = {
  "wifi",
  "wl",
  "wlan",
  "radio"
}
local netname = "wireless"
local ifcfgname = "wlan"
local community = "profile_"..uci:get("freifunk", "community", "name")
local sharenet = uci:get("ffwizard", "settings", "sharenet")

f = SimpleForm("ffwizard", "", "")
f.submit = "Save and reboot"
f.cancel = "Back"
f.reset = false

css = f:field(DummyValue, "css", "")
css.template = "freifunk/assistent/snippets/css"

-- ADHOC
meshipinfo = f:field(DummyValue, "meshinfo", "")
meshipinfo.template = "freifunk/assistent/snippets/meshipinfo"

local wifi_tbl = {}
uci:foreach("wireless", "wifi-device",
  function(section)
    local device = section[".name"]
    local channel = tonumber(section["channel"])
    local devicename
    if ( channel <= 14 ) then
      devicename = "2.4 Ghz Wifi ("..device:upper()..")"
    else
      devicename = "5 Ghz Wifi ("..device:upper()..")"
    end
    f:field(DummyValue, device:upper(), devicename)
    wifi_tbl[device] = {}
    local meship = f:field(Value, "meship_" .. device, "Mesh-IP", "")
    meship.rmempty = false
    meship.datatype = "ip4addr"
    function meship.cfgvalue(self, section)
      return uci:get("ffwizard", "settings", "meship_" .. device)
    end
    function meship.validate(self, value)
      local x = ip.IPv4(value or "")
      return ( x and x:is4()) and x:string() or ""
    end
    wifi_tbl[device]["meship"] = meship

    local supportedModes = tools.wifi_get_mesh_modes(device)
    local meshmode = f:field(ListValue, "mode_" .. device, "Mesh Mode", "")
    meshmode.widget = "radio"
    if supportedModes["80211s"] == true then
      meshmode:value("80211s", "802.11s")
      meshmode.default = "80211s"
    end
    if supportedModes["adhoc"] == true then
      meshmode:value("adhoc", "Ad-Hoc (veraltet)")
      if supportedModes["80211s"] ~= true then
        meshmode.default = "adhoc"
      end
    end
    function meshmode.cfgvalue(self, section)
      return uci:get("ffwizard", "settings", "meshmode_" .. device)
    end
    wifi_tbl[device]["meshmode"] = meshmode

  end)

-- VAP
local vap = uci:get_first(community, "community", "vap") or "1"
if vap == "1" then
  ipinfo = f:field(DummyValue, "ipinfo", "")
  ipinfo.template = "freifunk/assistent/snippets/ipinfo"

  ssid = f:field(Value, "ssid", "Freifunk-SSID", "")
  ssid.rmempty = false
  function ssid.cfgvalue(self, section)
    return uci:get("ffwizard", "settings", "ssid")
      or uci:get(community, "profile", "ssid")
  end

  dhcpmesh = f:field(Value, "dhcpmesh", "DHCP-Network", "")
  dhcpmesh.rmempty = false
  dhcpmesh.datatype = "ip4addr"
  function dhcpmesh.cfgvalue(self, section)
    return uci:get("ffwizard","settings", "dhcpmesh")
  end
  function dhcpmesh.validate(self, value)
    local x = ip.IPv4(value or "")
    return ( x and x:minhost() and x:prefix() < 32) and x:string() or ""
  end
end

main = f:field(DummyValue, "netconfig", "", "")
main.forcewrite = true
function main.parse(self, section)
  local fvalue = "1"
  if self.forcewrite then
    self:write(section, fvalue)
  end
end
function main.write(self, section, value)
  if (sharenet == "2") then
    --share internet was not enabled before, set to false now
    uci:set("ffwizard", "settings", "sharenet", "0")
    uci:save("ffwizard")
  end

  -- store wizard data to fill fields if wizard is rerun
  uci:set("ffwizard", "settings", "ssid", ssid:formvalue(section))
  uci:set("ffwizard", "settings", "dhcpmesh", dhcpmesh:formvalue(section))

  if (string.len(ssid:formvalue(section)) == 0
    or string.len(dhcpmesh:formvalue(section)) == 0) then
    -- form is not valid
    return
  end

  local statistics_installed = ipkg.installed("luci-app-statistics") == true
  local mergeList = {"freifunk", community}
  uci:foreach("wireless", "wifi-device",
    function(sec)
      local device = sec[".name"]

      -- store wizard data to fill fields if wizard is rerun
      uci:set("ffwizard", "settings",
        "meship_" .. device, wifi_tbl[device]["meship"]:formvalue(section)
      )
      uci:set("ffwizard", "settings",
        "meshmode_" .. device, wifi_tbl[device]["meshmode"]:formvalue(section)
      )

      if (string.len(wifi_tbl[device]["meship"]:formvalue(section)) == 0) then
        -- form is not valid
        return
      end

      cleanup(device)

      --OLSR CONFIG device
      local olsrifbase = {}
      olsrifbase.interface = calcnif(device)
      olsrifbase.ignore = "0"
      uci:section("olsrd", "Interface", nil, olsrifbase)

      --OLSR6 CONFIG device
      local olsrifbase6 = {}
      olsrifbase6.interface = calcnif(device)
      olsrifbase6.ignore = "0"
      uci:section("olsrd6", "Interface", nil, olsrifbase6)

      --FIREWALL CONFIG device
      tools.firewall_zone_add_interface("freifunk", calcnif(device))

      --WIRELESS CONFIG device
      local hwmode = calchwmode(device)
      local deviceSection = (hwmode:find("a")) and "wifi_device_5" or "wifi_device"
      local devconfig = tools.getMergedConfig(mergeList, "defaults", deviceSection)
      local devchannel = getchannel(device)
      devconfig.channel = tostring(devchannel)
      devconfig.hwmode = hwmode
      devconfig.doth = calcdoth(devchannel)
      devconfig.htmode = "HT20"
      devconfig.chanlist = calcchanlist(devchannel)
      uci:tset("wireless", device, devconfig)

      --WIRELESS CONFIG mesh
      local meshmode = wifi_tbl[device]["meshmode"]:formvalue(section)
      local pre = calcpre(devchannel)
      local ifaceSection
      if meshmode ~= "adhoc" then
         ifaceSection = "wifi_iface_"..meshmode
      else
         ifaceSection = ((pre == 2) and "wifi_iface" or "wifi_iface_5")
      end
      local ifconfig = tools.getMergedConfig(mergeList, "defaults", ifaceSection)
      local ifnameMesh = calcifcfg(device).."-"..ifconfig.mode.."-"..pre
      local secName = calcifcfg(device).."_"..ifconfig.mode.."_"..pre
      ifconfig.device = device
      ifconfig.network = calcnif(device)
      ifconfig.ifname = ifnameMesh
      if meshmode == "adhoc" then
        ifconfig.ssid = uci:get(community, "ssidscheme", devconfig.channel)
        ifconfig.bssid = uci:get(community, "bssidscheme", devconfig.channel)
      end
      uci:section("wireless", "wifi-iface", secName, ifconfig)
      if statistics_installed then
        tools.statistics_interface_add("collectd_iwinfo", ifnameMesh)
        tools.statistics_interface_add("collectd_interface", ifnameMesh)
      end

      --NETWORK CONFIG mesh
      local node_ip = wifi_tbl[device]["meship"]:formvalue(section)
      node_ip = ip.IPv4(node_ip)
      local prenetconfig = {}
      prenetconfig.ipaddr = node_ip:host():string()
      prenetconfig.netmask = uci:get(community,'interface','netmask')
      prenetconfig.ip6assign = 64
      prenetconfig.proto = "static"
      uci:section("network", "interface", calcnif(device), prenetconfig)

      --WIRELESS CONFIG ap
      if vap == "1" then
        local ifnameAp = calcifcfg(device).."-dhcp-"..pre
        local secName = calcifcfg(device).."_dhcp_"..pre
        uci:section("wireless", "wifi-iface", secName, {
          device=device,
          mode="ap",
          encryption="none",
          network="dhcp",
          ifname=ifnameAp,
          ssid=ssid:formvalue(section)
        })
        if statistics_installed then
          tools.statistics_interface_add("collectd_iwinfo", ifnameAp)
        end
      end

      uci:save("firewall")
      uci:save("olsrd")
      uci:save("olsrd6")
      uci:save("wireless")
      uci:save("network")
      if statistics_installed then
        uci:save("luci_statistics")
      end

    end)

  -- Set the dns entry on the loopback interface. We set it on the loopback
  -- interface so we only have one entry for the whole network configuration.
  local dns = uci:get(community, "interface", "dns")
  if (dns) then
    uci:set("network", "loopback", "dns", dns)
  end

  local dhcpmeshnet = dhcpmesh:formvalue(section)
  dhcpmeshnet = ip.IPv4(dhcpmeshnet)

  --only do this if user entered cidr
  if (dhcpmeshnet:prefix() < 32) then
    --NETWORK CONFIG bridge for wifi APs
    local prenetconfig =  {}
    prenetconfig.ipaddr=dhcpmeshnet:minhost():string()
    prenetconfig.netmask=dhcpmeshnet:mask():string()
    prenetconfig.ip6assign="64"
    prenetconfig.type="bridge"
    prenetconfig.proto="static"
    -- use ifname from dhcp bridge on a consecutive run of assistent
    prenetconfig.ifname=uci:get("network", "lan", "ifname") or uci:get("network", "dhcp", "ifname")

    -- if macaddr is set for lan interface also set it for dhcp interface (needed for wdr4900)
    local macaddr=uci:get("network", "lan", "macaddr") or uci:get("network", "dhcp", "macaddr")
    if (macaddr) then
      prenetconfig.macaddr = macaddr
    end

    uci:section("network", "interface", "dhcp", prenetconfig)

    -- add to statistics
    if statistics_installed then
      tools.statistics_interface_add("collectd_interface", "br-dhcp")
    end

    --NETWORK CONFIG remove lan bridge because ports a part of dhcp bridge now
    uci:delete("network", "lan")
    uci:delete("dhcp", "lan")

    --DHCP CONFIG change ip of frei.funk domain
    uci:set("dhcp", "frei_funk", "ip", dhcpmeshnet:minhost():string())

    --DHCP CONFIG bridge for wifi APs
    local dhcpbase = tools.getMergedConfig(mergeList, "defaults", "dhcp")
    dhcpbase.interface = "dhcp"
    dhcpbase.force = "1"
    dhcpbase.ignore = "0"
    uci:section("dhcp", "dhcp", "dhcp", dhcpbase)
    uci:set_list("dhcp", "dhcp", "dhcp_option", "119,olsr")
    uci:set("dhcp", "dhcp", "dhcpv6", "server")
    uci:set("dhcp", "dhcp", "ra", "server")
    -- DHCP CONFIG set start and limit option
    -- start (offset from network address) is 2
    -- first host address is used by the router
    -- limit is 2 ^ ( 32 - prefix) - 3
    -- do not assign broadcast address to dhcp clients
    local start = "2"
    local limit = tostring(math.pow(2, 32 - dhcpmeshnet:prefix()) - 3)
    uci:set("dhcp", "dhcp", "start", start)
    uci:set("dhcp", "dhcp", "limit", limit)

    --OLSR CONFIG announce dhcp bridge subnet (HNA)
    uci:section("olsrd", "Hna4", nil, {
      netmask = dhcpmeshnet:mask():string(),
      netaddr = dhcpmeshnet:network():string()
    })
  end

  uci:save("dhcp")
  uci:save("olsrd")
  uci:save("olsrd6")
  uci:save("network")
  uci:save("ffwizard")
end

function f.on_cancel()
  luci.http.redirect(luci.dispatcher.build_url("admin/freifunk/assistent/optionalConfigs"))
end

function f.handle(self, state, data)
  --how can I read form data here to get rid of this main field??
  if state == FORM_VALID then
    luci.http.redirect(luci.dispatcher.build_url("admin/freifunk/assistent/applyChanges"))
  end
end

function calcpre(channel)
  -- calculates suffix of wifi interface (like 2 or 5)
  return (channel > 0 and channel <= 14) and 2 or 5
end

function calcchanlist(channel)
  local chanlist
  if channel >= 100 and channel <= 140 then
    chanlist = "100 104 108 112 116 120 124 128 132 136 140"
  else
    chanlist =""
  end
  return chanlist
end

function calcdoth(channel)
  -- doth activates 802.11h (radar detection)
  return (channel >= 52 and channel <= 140) and "1" or "0"
end

function get_iwinfo(device)
  local iwinfo = require "iwinfo"
  local backend = iwinfo.type(device)
  return iwinfo[backend]
end

function calchwmode(device)
  local hwmode = "11"
  local iwinfo = get_iwinfo(device)

  for k,v in pairs(iwinfo.hwmodelist(device)) do
    if v then
      hwmode = hwmode .. k
    end
  end

  return hwmode
end

function getchannel(device)
  local iwinfo = get_iwinfo(device)
  local freqlist = iwinfo.freqlist(device)

  local r_channel
  if (freqlist[1].mhz > 2411 and freqlist[1].mhz < 2484) then
    --this is 2.4 Ghz
    r_channel = tonumber(uci:get(community, "wifi_device", "channel")) or 13
  end
  if (freqlist[1].mhz > 5179 and freqlist[1].mhz < 5701) then
    --this is 5 Ghz
    r_channel = tonumber(uci:get(community, "wifi_device_5", "channel")) or 36
  end
  tools.logger("channel for device "..device.." is "..tostring(r_channel))
  return r_channel
end

function calcnif(device)
  local nif
  for i, v in ipairs(device_l) do
    if string.find(device, v) then
      nif = string.gsub(device, v, netname)
    end
  end
  return nif
end

function calcifcfg(device)
  local ifcfg
  for i, v in ipairs(device_l) do
    if string.find(device, v) then
      ifcfg = string.gsub(device, v, ifcfgname)
    end
  end
  return ifcfg
end

function cleanup(device)
  tools.wifi_delete_ifaces(device)
  tools.wifi_delete_ifaces("wlan")
  uci:delete("network", device .. "dhcp")
  uci:delete("network", device)
  local nif = calcnif(device)
  tools.firewall_zone_remove_interface("freifunk", device)
  tools.firewall_zone_remove_interface("freifunk", nif)
  uci:delete_all("luci_splash", "iface", {network=device.."dhcp", zone="freifunk"})
  uci:delete_all("luci_splash", "iface", {network=nif.."dhcp", zone="freifunk"})
  uci:delete("network", nif .. "dhcp")
  uci:delete("network", nif)
  uci:delete("dhcp", device)
  uci:delete("dhcp", device .. "dhcp")
  uci:delete("dhcp", nif)
  uci:delete("dhcp", nif .. "dhcp")
end

return f
