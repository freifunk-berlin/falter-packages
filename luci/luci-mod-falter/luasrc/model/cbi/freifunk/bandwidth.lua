local uci = require "luci.model.uci".cursor()

m = Map("ffwizard", translate("Bandwidth Settings"), nil)

f = m:section(NamedSection, "settings", "settings",
  translate("Bandwidth Settings"),
  translate("The nodes of the Freifunk network foward data based on " ..
            "shortest paths and highest bandwidth. Therefor we need " ..
            "to know, how much bandwidth to the internet there is and " ..
            "how much do you want to share. Typical values are 6.0 " ..
            "Mbit/s download and 0.5 Mbit/s upload with <em>DSL 6000</em> " ..
            "or 50.0 Mbit/s download and 10.0 Mbit/s upload with " ..
            "<em>VDSL 50000</em>. Preferably you test the actual bandwidth " ..
            "multiple time with a tool like <em>speedof.me</em>. Then you " ..
            "can fill in how much bandwidth you are willing to share with " ..
            "your peers. Please be generous, but don't overestimate. :-)"))

local usersBandwidthDown = f:option(Value, "usersBandwidthDown",
    translate("Download-Bandwith in Mbit/s"))
usersBandwidthDown.datatype = "float"
usersBandwidthDown.rmempty = false

local usersBandwidthUp = f:option(Value, "usersBandwidthUp",
    translate("Upload-Bandwidth in Mbit/s"))
usersBandwidthUp.datatype = "float"
usersBandwidthUp.rmempty = false

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
  uci:set("ffwizard", "settings", "usersBandwidthUp", usersBandwidthUp:formvalue(section))
  uci:set("ffwizard", "settings", "usersBandwidthDown", usersBandwidthDown:formvalue(section))

  local up = usersBandwidthUp:formvalue(section) * 1000
  local down = usersBandwidthDown:formvalue(section) * 1000

  uci:set("qos", "ffuplink", "upload", up)
  uci:set("qos", "ffuplink", "download", down)
  local s = uci:get_first("olsrd", "olsrd")
  uci:set("olsrd", s, "SmartGatewaySpeed", up.." "..down)

  uci:save("ffwizard")
  uci:save("qos")
  uci:save("olsrd")
  uci:commit("ffwizard")
  uci:commit("qos")
  uci:commit("olsrd")

  -- Run the wizard again
  luci.http.redirect(luci.dispatcher.build_url("admin/freifunk/assistent/startWizard"))
end

return f
