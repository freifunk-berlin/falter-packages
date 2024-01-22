local uci = require "luci.model.uci".cursor()
local fs = require "nixio.fs"
local tools = require "luci.tools.freifunk.assistent.tools"

f = SimpleForm("ffuplink","","")
f.submit = "Next"
f.cancel = "Back"
f.reset = false

css = f:field(DummyValue, "css", "")
css.template = "freifunk/assistent/snippets/css"

shareBandwidth = f:field(DummyValue, "shareBandwidthfo", "")
shareBandwidth.template = "freifunk/assistent/snippets/shareBandwidth"

local usersBandwidthDown = f:field(Value, "usersBandwidthDown", translate("Download bandwidth in Mbit/s"))
usersBandwidthDown.datatype = "float"
usersBandwidthDown.rmempty = false
function usersBandwidthDown.cfgvalue(self, section)
  return uci:get("ffwizard", "settings", "usersBandwidthDown")
end

local usersBandwidthUp = f:field(Value, "usersBandwidthUp", translate("Upload bandwidth in Mbit/s"))
usersBandwidthUp.datatype = "float"
usersBandwidthUp.rmempty = false
function usersBandwidthUp.cfgvalue(self, section)
  return uci:get("ffwizard", "settings", "usersBandwidthUp")
end

main = f:field(DummyValue, "shareInternet", "", "")
main.forcewrite = true

function main.parse(self, section)
  local fvalue = "1"
  if self.forcewrite then
    self:write(section, fvalue)
  end
end

function main.write(self, section, value)
  uci:set("ffwizard", "settings", "sharenet", 1)
  uci:set("ffwizard", "settings", "usersBandwidthUp", usersBandwidthUp:formvalue(section))
  uci:set("ffwizard", "settings", "usersBandwidthDown", usersBandwidthDown:formvalue(section))

  uci:save("ffwizard")
end

function f.handle(self, state, data)
  if state == FORM_VALID then
    luci.http.redirect(luci.dispatcher.build_url("admin/freifunk/assistent/optionalConfigs"))
  end
end

function f.on_cancel()
  luci.http.redirect(luci.dispatcher.build_url("admin/freifunk/assistent/decide"))
end

return f
