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

local usersBandwidthDown = f:field(Value, "usersBandwidthDown", translate("Download-Bandbreite in Mbit/s"))
usersBandwidthDown.datatype = "float"
usersBandwidthDown.rmempty = false
function usersBandwidthDown.cfgvalue(self, section)
  return uci:get("ffwizard", "settings", "usersBandwidthDown")
end

local usersBandwidthUp = f:field(Value, "usersBandwidthUp", translate("Upload-Bandbreite in Mbit/s"))
usersBandwidthUp.datatype = "float"
usersBandwidthUp.rmempty = false
function usersBandwidthUp.cfgvalue(self, section)
  return uci:get("ffwizard", "settings", "usersBandwidthUp")
end

if uci:get("ffberlin-uplink", "uplink", "auth") == "x509" then
  vpninfo = f:field(DummyValue, "vpninfo", "")
  vpninfo.template = "freifunk/assistent/snippets/vpninfo"
  if luci.http.formvalue("reupload", true) == "1" then
    fs.unlink("/etc/openvpn/ffuplink.crt")
    fs.unlink("/etc/openvpn/ffuplink.key")
    luci.http.redirect(luci.dispatcher.build_url("admin/freifunk/assistent/sharedInternet"))
  end
  if fs.access("/etc/openvpn/ffuplink.crt") then
    vpncertreupload = f:field(DummyValue, "reupload", "")
    vpncertreupload.template = "freifunk/assistent/snippets/vpncertreupload"
  else
    local cert = f:field(FileUpload, "cert", translate("Local Certificate"),"ffuplink.crt")
    cert.default="/etc/openvpn/ffuplink.crt"
    cert.rmempty = false
    cert.optional = false

    local key = f:field(FileUpload, "key", translate("Local Key"),"ffuplink.key")
    key.default="/etc/openvpn/ffuplink.key"
    key.rmempty = false
    key.optional = false
  end
end

main = f:field(DummyValue, "openvpnconfig", "", "")
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

  uci:section("openvpn", "openvpn", "ffuplink", {
    --persist_tun='0',
    enabled='1'
  })

  fs.move(
    "/etc/luci-uploads/cbid.ffuplink.1.cert",
    "/etc/openvpn/ffuplink.crt"
  )
  fs.move(
    "/etc/luci-uploads/cbid.ffuplink.1.key",
    "/etc/openvpn/ffuplink.key"
  )

  uci:save("openvpn")
  uci:save("ffwizard")

  -- I need to commit this here, don't know why I can not do this in apply changes
  uci:commit("openvpn")

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
