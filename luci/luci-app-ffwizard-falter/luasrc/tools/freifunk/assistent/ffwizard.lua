--[[
LuCI - Lua Configuration Interface

Copyright 2008 Steven Barth <steven@midlink.org>
Copyright 2008 Jo-Philipp Wich <xm@leipzig.freifunk.net>

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

$Id$

]]--

local uci = require "luci.model.uci".cursor()
local tools = require "luci.tools.freifunk.assistent.tools"
local table = require "table"
local type = type
local sharenet = uci:get("ffwizard", "settings", "sharenet")

module ("luci.tools.freifunk.assistent.ffwizard", package.seeall)

function configureQOS()
  if sharenet == "1" then
    -- values have to be in kilobits/second
    local up = uci:get("ffwizard", "settings", "usersBandwidthUp") * 1000
    local down = uci:get("ffwizard", "settings", "usersBandwidthDown") * 1000

    uci:delete("qos","wan")
    uci:delete("qos","lan")
    uci:delete("qos","ffuplink")
    uci:section("qos", 'interface', "ffuplink", {
      enabled = "1",
      classgroup = "Default",
      upload = up,
      download = down
    })

    local s = uci:get_first("olsrd", "olsrd")
    uci:set("olsrd", s, "SmartGatewaySpeed", up.." "..down)
    uci:set("olsrd", s, "SmartGatewayUplink", "both")
    uci:save("olsrd")
    uci:save("qos")
  end
end
