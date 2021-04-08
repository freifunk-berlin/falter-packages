local fftools = require "luci.tools.freifunk.assistent.tools"
local uci = require "luci.model.uci".cursor()

f = SimpleForm("wireless", translate("Update Wireless-Mesh Settings"),
  translate("The wireless interfaces which are built into the router can " ..
    "mesh with other routers using either Ad-Hoc or 802.11s. <strong>" ..
    "802.11s is now the standard used.</strong> Some routers (or " ..
    "their drivers) might not support this new standard. Also, if this " ..
    "router currently meshes with Ad-Hoc then changing it to 802.11s would " ..
    "cause connectivity loss. Please contact your mesh neighbors to switch " ..
    "to 802.11s collectively. Please select which protocol to use. <em>The " ..
    "menu entry <strong>Freifunk->Wireless Mesh</strong> can be visited at " ..
    "a later time to change these settings.</em>"))

local wifi_tbl = {}

uci:foreach("wireless", "wifi-device",
  function(section)
    -- get the Frequency of the device
    local device = section[".name"]
    wifi_tbl[device] = section
    local channel = tonumber(section["channel"])
    local devicename
    if ( channel <= 14 ) then
      devicename = "2.4 Ghz Wifi ("..device:upper()..")"
    else
      devicename = "5 Ghz Wifi ("..device:upper()..")"
    end

    -- determine which mesh modes are support by this radio

    -- find the wifi-iface which is either adhoc or mesh
    uci:foreach("wireless", "wifi-iface",
      function(ifaceSection)
        if ( device ~= ifaceSection["device"] ) then
          return
        end
        if ( "mesh" ~= ifaceSection["mode"] and "adhoc" ~= ifaceSection["mode"] ) then
          return
        end
        local meshmode = f:field(ListValue, "mode_" .. device, devicename, 
            translate("The " .. devicename .. " device is currently set to <strong>" .. 
            ((ifaceSection["mode"] == "adhoc") and "Ad-Hoc" or "802.11s") ..
            "</strong>. The current default setup is 802.11s.  Please select " ..
            "how to use this device in the future. "))
        meshmode.widget = "radio"
        local supportedModes = fftools.wifi_get_mesh_modes(device)
        if supportedModes["80211s"] == true then
          meshmode:value("80211s", "802.11s")
          meshmode.default = "80211s"
        end
        if supportedModes["adhoc"] == true then
          meshmode:value("adhoc", translate("Ad-Hoc (outdated)"))
          if supportedModes["80211s"] ~= true then
            meshmode.default = "adhoc"
          end
        end
        wifi_tbl[device]["oldmeshmode"] = ifaceSection["mode"]
        wifi_tbl[device]["newmeshmode"] = meshmode
      end)
  end)

-- "behind the scenes magic" to make the submit button do something
main = f:field(DummyValue, "netconfig", "", "")
main.forcewrite = true
function main.parse(self, section)
  local fvalue = "1"
  if self.forcewrite then
    self:write(section, fvalue)
  end
end
-- end of "behind the scenes magic"

-- write the new settings
function main.write(self, section, value)
  write_ffwizard(section)
  write_wireless(section)
  write_luci_statistics(section)

  -- Run the wizard again
  luci.http.redirect(luci.dispatcher.build_url("admin/freifunk/assistent/startWizard"))
end

function write_ffwizard(section)
  -- set the meshmode parameter(s)
  uci:foreach("wireless", "wifi-device",
    function(sec)
      local device = sec[".name"]
      uci:set("ffwizard", "settings","meshmode_" .. device,
        wifi_tbl[device]["newmeshmode"]:formvalue(section))
    end)

  -- mark this upgrade as the last upgrade
  uci:set("ffwizard","upgrade","lastUpgrade","005-wireless-mesh")
  uci:save("ffwizard")
  uci:commit("ffwizard")
end

function write_wireless(section)
  uci:foreach("wireless", "wifi-iface",
    function(sec)
      local name = sec[".name"]
      local mode = sec["mode"]
      local ifname = sec["ifname"]
      local device = sec["device"]
      local network = sec["network"]
      local formvalue = wifi_tbl[device]["newmeshmode"]:formvalue(section)
      local newmeshmode = formvalue
      if ( "80211s" == newmeshmode ) then
        newmeshmode = "mesh"
      end

      if ( "mesh" ~= mode and "adhoc" ~= mode  ) then
        return
      end
      if ( mode == newmeshmode ) then
        return
      end
 
      local mergeList = {"freifunk", "profile_"..uci:get("freifunk", "community", "name")}

      local ifaceDefault
      if formvalue ~= "adhoc" then
         ifaceDefault = "wifi_iface_"..formvalue
      else
         local pre = string.sub(ifname,-1)
         ifaceDefault = ((pre == "2") and "wifi_iface" or "wifi_iface_5")
      end
      local ifconfig = fftools.getMergedConfig(mergeList, "defaults", ifaceDefault)
      ifconfig.device = device
      ifconfig.network = network
      ifconfig.ifname = string.gsub(ifname, mode, newmeshmode)
      if ( newmeshmode == "adhoc" ) then
        local community = "profile_"..uci:get("freifunk", "community", "name")
        local devChannel = uci:get("wireless", device, "channel")
        ifconfig.ssid = uci:get(community, "ssidscheme", devChannel)
        ifconfig.bssid = uci:get(community, "bssidscheme", devChannel)
      end

      local newSectionName = string.gsub(name, mode, newmeshmode)

      -- delete the old section and replace it with a new one
      uci:delete("wireless", name)
      uci:section("wireless", "wifi-iface", newSectionName, ifconfig)

      -- RSSI LED setting
      local rssidev = string.sub(ifconfig.ifname,
                                 string.find(ifconfig.ifname, "wlan%d"))
      local rssiled = uci:get("system", "rssid_"..rssidev, "dev")
      if rssiled then
        uci:set("system", "rssid_"..rssidev, "dev", ifconfig.ifname)
      end
    end)

    uci:save("system")
    uci:save("wireless")
    uci:commit("system")
    uci:commit("wireless")

end

function write_luci_statistics(section)
  -- only make changes if statistics are installed
  local ipkg = require "luci.model.ipkg"
  if ( ipkg.installed("luci-app-statistics") ~= true ) then
    return
  end

  -- get the old list of interfaces
  local collectd_interface = uci:get("luci_statistics", "collectd_interface", "Interfaces")
  local collectd_iwinfo = uci:get("luci_statistics", "collectd_iwinfo", "Interfaces")

  -- update the list of interfaces
  uci:foreach("wireless", "wifi-iface",
    function(sec)
      local ifname = sec["ifname"]
      local device = sec["device"]
      local newmeshmode = sec["mode"]

      if ( "mesh" ~= newmeshmode and "adhoc" ~= newmeshmodemode  ) then
        return
      end 

      local oldmeshmode = wifi_tbl[device]["oldmeshmode"]
      if ( oldmeshmode == newmeshmode ) then
        return
      end 

      local oldifname = string.gsub(ifname, newmeshmode, oldmeshmode)
      oldifname = string.gsub(oldifname, "%-", "%%-") -- escape the '-'
      collectd_interface, x = string.gsub(collectd_interface, oldifname, ifname)
      collectd_iwinfo, y = string.gsub(collectd_iwinfo, oldifname, ifname)

    end)

  -- write the changes
  uci:set("luci_statistics", "collectd_interface", "Interfaces", collectd_interface)
  uci:set("luci_statistics", "collectd_iwinfo", "Interfaces", collectd_iwinfo)
  uci:save("luci_statistics")
  uci:commit("luci_statistics")
end

return f


