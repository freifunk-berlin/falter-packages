local util = require "luci.util"
local uci = require "luci.model.uci".cursor()
local ip = require "luci.ip"
local tools = require "luci.tools.freifunk.assistent.tools"

local sharenet = uci:get("ffwizard","settings","sharenet")
local community = "profile_"..uci:get("freifunk", "community", "name")

module "luci.tools.freifunk.assistent.olsr"

function prepareOLSR()
	local c = uci.cursor()
	uci:delete_all("olsrd", "Interface")
	uci:delete_all("olsrd", "Hna4")

	uci:save("olsrd")
end


function configureOLSR()
	local mergeList = {"freifunk", community}
	-- olsr 4
	local olsrbase = tools.getMergedConfig(mergeList, "defaults", "olsrd")
	tools.mergeInto("olsrd", "olsrd", olsrbase)

  -- olsr 4 interface defaults
  local olsrifbase = tools.getMergedConfig(mergeList, "defaults", "olsr_interface")
  tools.mergeInto("olsrd", "InterfaceDefaults", olsrifbase)

  uci:save("olsrd")
end


function configureOLSRPlugins()
	local suffix = uci:get_first(community, "community", "suffix") or "olsr"
	updatePlugin("olsrd_nameservice", "suffix", "."..suffix)
	updatePluginInConfig("olsrd", "olsrd_dyn_gw", "PingCmd", "ping -c 1 -q -I ffuplink %s")
	updatePluginInConfig("olsrd", "olsrd_dyn_gw", "PingInterval", "30")
	uci:save("olsrd")
end


function updatePluginInConfig(config, pluginName, key, value)
	uci:foreach(config, "LoadPlugin",
		function(plugin)
			if (plugin.library == pluginName) then
				uci:set(config, plugin['.name'], key, value)
			end
		end)
end


function updatePlugin(pluginName, key, value)
	updatePluginInConfig("olsrd", pluginName, key, value)
end
