local uci = require "luci.model.uci".cursor()

function get_device_ports(name)
  ports = {}

  uci:foreach("network", "device",
    function(sec)
      if ( sec["name"] == name ) then
        ports = uci:get_list("network", sec[".name"], "ports")
        return
      end
  end)

  return ports
end

function set_device_ports(name, value)
  uci:foreach("network", "device",
    function(sec)
      if ( sec["name"] == name ) then
        uci:set_list("network", sec[".name"], "ports", value)
        return
      end
   end)
end

local wanports = get_device_ports("br-wan")
local has_ffuplink_wan = 0
for key, port in ipairs(wanports) do
  if port == "ffuplink_wan" then
    has_ffuplink_wan = 1
    table.remove(wanports, key)
  end
end
local dhcpports = get_device_ports("br-dhcp")

m = Map("ffwizard", translate("Swap WAN and DCHP Physical Ports"), nil)

f = m:section(NamedSection, "settings", "settings",
  translate("Swap WAN and DHCP Physical Ports"),
  translate("Sometimes the physical ports on a router are inconveniently " ..
    "assigned to WAN and DHCP networks.  By selecting to swap the ports, " ..
    "the roles of the ports will be reversed.<br><ul><li>A router with " ..
    "only two ports will reverse their roles.  An example is the " ..
    "NanoStation M2/M5.  The standard setup for the PoE port is DHCP, " ..
    "while WAN needs to be connected to the secondary port.  This " ..
    "is a quite inconvenient configuration which this tool will change." ..
    "<li>A router with only one port will, per default, be assigned to " ..
    "DHCP.  This prevents being able to have any WAN access.  This tool " ..
    "will reassign the single physical port to WAN.  An example router is " ..
    "the Unifi-AC-Mesh.<li>A standand home router, with normally one WAN " ..
    "port and multiple DHCP ports can benefit from this tool as well. " ..
    "For example, if you want to use those ports for your private network " ..
    "(the network which has your DSL router) to be able to add additional " ..
    "devices (printer, game console, NAS storage), this tool will enable " ..
    "that.  In this setup, the single port will be the only access to the " ..
    "freifunk network, while the remaining ports will be a part of your " ..
    "home network as well as internet access for freifunk</ul><br><br> " ..
    "Current WAN ports are: ") .. table.concat(wanports, ", ") ..
    translate("<br> Current DHCP ports are: ") .. table.concat(dhcpports, ", ")
    )

o = f:option(Flag, "swapports", translate("Swap WAN and DCHP ports"), nil)

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
  local status = tonumber(o:formvalue(section))

  if status == 1 then
    -- add ffuplink_wan to the future br-wan
    if has_ffuplink_wan == 1 then
      table.insert(dhcpports, "ffuplink_wan")
    end
    -- do the swap
    set_device_ports("br-wan", dhcpports)
    set_device_ports("br-dhcp", wanports)
  end

  uci:revert("ffwizard")
  uci:save("network")
  uci:commit("network")

  -- Run the wizard again
  luci.http.redirect(luci.dispatcher.build_url("admin/freifunk/assistent/startWizard"))
end

return f
