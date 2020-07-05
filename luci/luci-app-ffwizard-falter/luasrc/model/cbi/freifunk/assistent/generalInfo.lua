local uci = require "luci.model.uci".cursor()
local sys = require "luci.sys"
local fs = require "nixio.fs"
local tools = require "luci.tools.freifunk.assistent.tools"

f = SimpleForm("ffwizward", "", "")
f.submit = "Next"
f.reset = false

css = f:field(DummyValue, "css", "")
css.template = "freifunk/assistent/snippets/css"

generalinfo = f:field(DummyValue,"","")
generalinfo.template = "freifunk/assistent/snippets/generalInfo"

community = f:field(ListValue, "net", "Freifunk-Community", "")
function community.cfgvalue(self, section)
  return uci:get("freifunk", "community", "name") or "berlin"
end
local profiles = "/etc/config/profile_"
for v in fs.glob(profiles.."*") do
  local n = string.gsub(v, profiles, "")
  local name = uci:get_first("profile_"..n, "community", "name") or "?"
  community:value(n, name)
end

hostname = f:field(Value, "hostname", "Name dieses Freifunk-Knotens", "")
hostname.datatype = "hostname"
function hostname.cfgvalue(self, section)
  return uci:get_first("system", "system","hostname") or sys.hostname()
end

nickname = f:field(Value, "nickname", "Dein Nickname","")
nickname.datatype = "string"
function nickname.cfgvalue(self, section)
  return uci:get("freifunk", "contact", "nickname")
end

realname = f:field(Value, "realname", "Dein Realname","")
realname.datatype = "string"
function realname.cfgvalue(self, section)
  return uci:get("freifunk", "contact", "name")
end

mail = f:field(Value, "mail", "E-Mail", "")
mail.datatype = "string"
function mail.cfgvalue(self, section)
  return uci:get("freifunk", "contact", "mail")
end

location = f:field(Value, "location", "Standort", "")
location.datatype = "string"
function location.cfgvalue(self, section)
  return uci:get_first("system", "system", "location") or uci:get("freifunk", "contact", "location")
end

lat = f:field(Value, "lat", "Geographischer Breitengrad", "")
lat.datatype = "float"
function lat.cfgvalue(self, section)
  return uci:get_first("system", "system","latitude")
end

lon = f:field(Value, "lon", "Geographischer Längengrad", "")
lon.datatype = "float"
function lon.cfgvalue(self, section)
  return uci:get_first("system", "system","longitude")
end

alt = f:field(Value, "alt", "Höhe über Grund", "")
alt.datatype = "float"
function alt.cfgvalue(self, section)
  return uci:get_first("system", "system","altitude")
end

map = f:field(DummyValue,"","")
map.template = "freifunk/assistent/snippets/map"

main = f:field(DummyValue, "config", "", "")
main.forcewrite = true
function main.parse(self, section)
  local fvalue = "1"
  if self.forcewrite then
    self:write(section, fvalue)
  end
end
function main.write(self, section, value)
  uci:set("freifunk", "contact", "nickname", nickname:formvalue(section))
  uci:set("freifunk", "contact", "name", realname:formvalue(section))
  uci:set("freifunk", "contact", "mail", mail:formvalue(section))
  uci:set("freifunk", "contact", "location",location:formvalue(section))

  local selectedCommunity = community:formvalue(section) or "Freifunk"
  local mergeList= {"profile_"..selectedCommunity}
  local profileData = tools.getMergedConfig(mergeList, "community", "profile")
  for key, val in pairs(profileData) do
    uci:set("freifunk", "community", key, val)
  end
  uci:set("freifunk", "community", "name", selectedCommunity)

  local latval
  local lonval
  if (lat:formvalue(section) and lon:formvalue(section)) then
    latval = tonumber(lat:formvalue(section))
    lonval = tonumber(lon:formvalue(section))
  end
  local altval
  if (alt:formvalue(section)) then
    altval = tonumber(alt:formvalue(section))
  end

  --SYSTEM CONFIG
  uci:foreach("system", "system",
    function(s)
      uci:set("system", s[".name"], "cronloglevel", "10")
      uci:set("system", s[".name"], "zonename", "Europe/Berlin")
      uci:set("system", s[".name"], "timezone", 'CET-1CEST,M3.5.0,M10.5.0/3')
      uci:set("system", s[".name"], "hostname", hostname:formvalue(section))
      if (lonval and latval) then
        uci:set("system", s[".name"], "latitude",string.format("%.15f", latval))
        uci:set("system", s[".name"], "longitude",string.format("%.15f", lonval))
      else
        uci:delete("system", s[".name"], "latitude")
        uci:delete("system", s[".name"], "longitude")
      end
      if altval then
        uci:set("system", s[".name"], "altitude",string.format("%.15f", altval))
      else
        uci:delete("system", s[".name"], "altitude")
      end
      uci:set("system", s[".name"], "location",location:formvalue(section))

    end)


  uci:save("system")
  uci:commit("system")
  uci:save("freifunk")
  uci:commit("freifunk")
end

function f.handle(self, state, data)
        if state == FORM_VALID then
          luci.http.redirect(luci.dispatcher.build_url("admin/freifunk/assistent/decide"))
        end
end

function f.on_cancel()
        luci.http.redirect(luci.dispatcher.build_url("admin/freifunk/assistent/cancel"))
end

return f
