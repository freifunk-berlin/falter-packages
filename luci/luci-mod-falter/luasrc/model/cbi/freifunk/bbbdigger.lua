local sys = require "luci.sys"
local uci = require "luci.model.uci".cursor()

m = Map("ffwizard", translate("BBB-VPN (bbbdigger) Settings"), nil)

f = m:section(NamedSection, "settings", "settings",
  translate("BBB-VPN (bbbdigger) Settings"),
  translate("To enable island nodes of the Berlin Freifunk Network " ..
    "to be able to virtually mesh with the Berlin Backbone (BBB), " ..
    "BBB-VPN servers have been set up to allow meshing through " ..
    "a VPN using tunneldigger (bbbdigger).  The BBB-VPN servers run " ..
    "a modified version of OLSRd which blocks any gateway advertisements " ..
    "and does not allow the VPN clients to see each other as direct " ..
    "neighbors.<br><br>Using bbbdigger will add a constant amount of " ..
    "background traffic going over the WAN interface of the router. So " ..
    "it is not recommended for devices which have metered internet access."))

-- origStatus 1=disabled 0=enabled nil=not found (treat as disabled)
local origStatus = uci:get("network", "bbbdigger", "disabled")
local oldStatus = origStatus or "1"

local status = f:option(ListValue, "bbbdigger",
               translate("Conntecting to the BBB-VPN"),
               translate("Upon submission you will be redirected to the " ..
                         "OLSRd Neighbors page.  There you will be able " ..
                         "to see the effects of enabling/disabling " ..
                         "bbbdigger.  It may take up to 60 seconds after " ..
                         "enabling to see any results"))
status.widget = "radio"
status:value(0, "enabled")
status:value(1, "disabled")
status.default = oldStatus

-- "behind the scenes magic" to make the submit button do something
main = f:option(DummyValue, "netconfig", "", "")
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
  local oldStatus = tonumber(oldStatus)
  local newStatus = tonumber(status:formvalue(section))

  if (oldStatus ~= newStatus) then
    if (newStatus == 1) then 
      -- bbbdigger must have been set up in the past, just disable it
      -- and restart tunneldigger to take down the l2tp interface
      uci:set("network", "bbbdigger", "disabled", "1")
      uci:set("tunneldigger", "bbbdigger", "enabled", "0")
      uci:set("olsrd", "bbbdigger", "ignore", "1")
    elseif (origStatus ~= nil) then -- reenable
      -- bbbdigger has been set up before, then disabled.  Simply reenable it
      uci:set("network", "bbbdigger", "disabled", "0")
      uci:set("tunneldigger", "bbbdigger", "enabled", "1")
      uci:set("olsrd", "bbbdigger", "ignore", "0")
    else
      -- create the device
      local mac = "b6" -- start with b6 for Berlin 6ackbone
      for byte=2,6 do
        mac = mac .. sys.exec("dd if=/dev/urandom bs=1 count=1 2> /dev/null | hexdump -e '1/1 \":%02x\"'")
      end
      uci:set("network", "bbbdigger_dev", "device")
      uci:set("network", "bbbdigger_dev", "macaddr", mac)
      uci:set("network", "bbbdigger_dev", "name", "bbbdigger")

      -- create the interface
      uci:set("network", "bbbdigger", "interface")
      uci:set("network", "bbbdigger", "proto", "dhcp")
      uci:set("network", "bbbdigger", "device", "bbbdigger")
      uci:set("network", "bbbdigger", "disabled", "0")

      -- create the tunneldigger section
      local uuid = mac
      for byte=7,10 do
        uuid = uuid .. sys.exec("dd if=/dev/urandom bs=1 count=1 2> /dev/null | hexdump -e '1/1 \":%02x\"'")
      end
      uci:set("tunneldigger", "bbbdigger", "broker")
      uci:set("tunneldigger", "bbbdigger", "srv", "_bbb-vpn._udp.berlin.freifunk.net")
      uci:set("tunneldigger", "bbbdigger", "uuid", uuid)
      uci:set("tunneldigger", "bbbdigger", "interface", "bbbdigger")
      uci:set("tunneldigger", "bbbdigger", "broker_selection", "usage")
      uci:set("tunneldigger", "bbbdigger", "bind_interface", "wan")
      uci:set("tunneldigger", "bbbdigger", "enabled", "1")

      -- add bbbdigger to the freifunk firewall zone
      local fwzone = uci:get("firewall", "zone_freifunk", "network")
      table.insert(fwzone, "bbbdigger")
      uci:set("firewall", "zone_freifunk", "network", fwzone)

      -- add bbbdigger to olsrd
      local olsrif = uci:set("olsrd", "bbbdigger", "Interface")
      uci:set("olsrd", "bbbdigger", "ignore", "0")
      uci:set("olsrd", "bbbdigger", "interface", "bbbdigger")
      uci:set("olsrd", "bbbdigger", "Mode", "ether")
    end

    -- wrap it up
    uci:save("network")
    uci:save("tunneldigger")
    uci:save("firewall")
    uci:save("olsrd")
    uci:commit("network")
    uci:commit("tunneldigger")
    uci:commit("firewall")
    uci:commit("olsrd")
    sys.exec("/etc/init.d/tunneldigger restart bbbdigger")

  end

  -- don't save to ffwizard.settings.bbbdigger
  uci:revert("ffwizard")
  
  luci.http.redirect(luci.dispatcher.build_url("olsr/neighbours"))
end

return f

