local sys = require "luci.sys"
local util = require "luci.util"
local uci = require "luci.model.uci".cursor()

module("luci.tools.freifunk.assistent.tools", package.seeall)

function wifi_get_mesh_modes(device)
	local modes = {}
	local phy = string.gsub(device, "radio", "phy")
	local iwOutput = sys.exec("iw phy "..phy.." info | grep -A1 \"valid interface combination\" | tail -1")
	if string.find(iwOutput, "mesh point") then
		modes["80211s"] = true;
	end
	if string.find(iwOutput, "IBSS") then
		modes["adhoc"] = true;
	end

	return modes
end

-- Deletes all references of a wifi device
function wifi_delete_ifaces(device)
	local cursor = uci.cursor()
	cursor:delete_all("wireless", "wifi-iface", {device=device})
	cursor:save("wireless")
end


function statistics_interface_add(mod, interface)
	local c = uci.cursor()
	local old = c:get("luci_statistics", mod, "Interfaces")
	c:set("luci_statistics", mod, "Interfaces", (old and old .. " " or "") .. interface)
	c:save("luci_statistics")
end

-- Adds interface to zone, creates zone on-demand
function firewall_zone_add_interface(name, interface)
	local cursor = uci.cursor()
	local zone = firewall_find_zone(name)
	local net = cursor:get("firewall", zone, "network")
	cursor:set("firewall", zone, "network", add_list_entry(net, interface))
	cursor:save("firewall")
end


-- Removes interface from zone
function firewall_zone_remove_interface(name, interface)
	local cursor = uci.cursor()
	local zone = firewall_find_zone(name)
	if zone then
		local net = cursor:get("firewall", zone, "network")
		local new = remove_list_entry(net, interface)
		if new then
			if #new > 0 then
				cursor:set("firewall", zone, "network", new)
			else
				cursor:delete("firewall", zone, "network")
			end
			cursor:save("firewall")
		end
	end
end


-- Finds the firewall zone with given name
function firewall_find_zone(name)
	local find

	uci.cursor():foreach("firewall", "zone",
		function (section)
			if section.name == name then
				find = section[".name"]
			end
		end)

	return find
end


-- checks if root-password has been set via CGI has_root-pass 
function hasRootPass()
	local jsonc = require "luci.jsonc"
	local isPasswordSet = true

	local f = io.popen("wget http://localhost/ubus -q -O - --post-data '{ \"jsonrpc\": \"2.0\", \"method\": \"call\", \"params\": [ \"00000000000000000000000000000000\", \"ffwizard-berlin\", \"has_root-pass\", {} ] }'")
	local ret = f:read("*a")
	f:close()

	local content = jsonc.parse(ret)
	local result = content.result
	local test = result[2]
	logger ("checking for root-password ..." .. test.password_is_set)

	if test.password_is_set == "no" then
		isPasswordSet = false
	end
	return isPasswordSet
end


-- Helpers --
-- Adds an entry to a table, always returns a table
function add_list_entry(value, entry)
	local newtable = {}
	if type(value) == "nil" then
		-- the table was empty, 
	elseif type(value) == "table" then
		-- make sure the value is not already in the table
		newtable = remove_list_entry(value, entry) or value
	else
		-- the "table" seems to be just a string, split it up
		newtable = util.split(value, " ")
	end

	table.insert(newtable, entry)
	return newtable
end

-- Removes a listentry, handles real and pseduo lists transparently
function remove_list_entry(value, entry)
	if type(value) == "nil" then
		return nil
	end

	local result = type(value) == "table" and value or util.split(value, " ")
	local key = util.contains(result, entry)

	while key do
		table.remove(result, key)
		key = util.contains(result, entry)
	end

	result = type(value) == "table" and result or table.concat(result, " ")
	return result ~= value and result
end


function logger(msg)
        sys.exec("logger -t ffwizard -p 5 '"..msg.."'")
end

--Merge the options of multiple config files into a table.
--
--configs: an array of strings, each representing a config file.  
--  The order is important since  the first config file is read, 
--  then the following.  Any options in the following config files
--  overwrite the values of any previous config files. 
--  e.g. {"freifunk", "profile_berlin"}
--sectionType: the section type to merge. e.g. "defaults"
--sectionName: the section to merge. e.g. "olsrd"
function getMergedConfig(configs, sectionType, sectionName)
	local data = {}
	for i, config in ipairs(configs) do
		uci:foreach(config, sectionType,
			function(s)
				if s['.name'] == sectionName then
					for key, val in pairs(s) do
						if string.sub(key, 1, 1) ~= '.' then
							data[key] = val
						end
					end
				end
			end)
		end
	return data
end

function mergeInto(config, section, options)
	local s = uci:get_first(config, section)
	if (section) then
		uci:tset(config, s, options)
	else
		uci:section(config, section, nil, options)
	end
	uci:save(config)
end
