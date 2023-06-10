#!/usr/bin/lua

--[[]
	Both functions, fetch_jsoninfo() and fetch_hna() are copied from
	/usr/lib/lua/luci/controller/olsr.lua.

	fetch_jsoninfo() is the exact same code as there.

	fetch_hna() is mainly the same as the function action_hna() there. In
	opposite, it does not call the render-function of LuCI and returns
	the fetched data in a table.
--]]

function fetch_jsoninfo(otable)
	local uci = require "luci.model.uci".cursor_state()
	local utl = require "luci.util"
	local json = require "luci.json"
	local IpVersion = uci:get_first("olsrd", "olsrd","IpVersion")
	local jsonreq4 = ""
	local jsonreq6 = ""
	local v4_port = uci:get("olsrd", "olsrd_jsoninfo", "port") or 9090
	local v6_port = uci:get("olsrd6", "olsrd_jsoninfo", "port") or 9090

	jsonreq4 = utl.exec("(echo /" .. otable .. " | nc 127.0.0.1 " .. v4_port .. ") 2>/dev/null")
	jsonreq6 = utl.exec("(echo /" .. otable .. " | nc ::1 " .. v6_port .. ") 2>/dev/null")
	local jsondata4 = {}
	local jsondata6 = {}
	local data4 = {}
	local data6 = {}
	local has_v4 = False
	local has_v6 = False

	if jsonreq4 == '' and jsonreq6 == '' then
		luci.template.render("status-olsr/error_olsr")
		return nil, 0, 0, true
	end

	if jsonreq4 ~= "" then
		has_v4 = 1
		jsondata4 = json.decode(jsonreq4)
		if otable == 'status' then
			data4 = jsondata4 or {}
		else
			data4 = jsondata4[otable] or {}
		end

		for k, v in ipairs(data4) do
			data4[k]['proto'] = '4'
		end

	end
	if jsonreq6 ~= "" then
		has_v6 = 1
		jsondata6 = json.decode(jsonreq6)
		if otable == 'status' then
			data6 = jsondata6 or {}
		else
			data6 = jsondata6[otable] or {}
		end
		for k, v in ipairs(data6) do
			data6[k]['proto'] = '6'
		end
	end

	for k, v in ipairs(data6) do
		table.insert(data4, v)
	end

	return data4, has_v4, has_v6, false
end


function fetch_hna()
	local data, has_v4, has_v6, error = fetch_jsoninfo('hna')
	if error then
		print("An error occured!")
		return
	end

	local uci = require "luci.model.uci".cursor_state()
	local resolve = uci:get("luci_olsr", "general", "resolve")

	local function compare(a,b)
		if a.proto == b.proto then
			return a.genmask < b.genmask
		else
			return a.proto < b.proto
		end
	end

	for k, v in ipairs(data) do
		if resolve == "1" then
			hostname = nixio.getnameinfo(v.gateway, nil, 100)
			if hostname then
				v.hostname = hostname
			end
		end
		if v.validityTime then
			v.validityTime = tonumber(string.format("%.0f", v.validityTime / 1000))
		end
	end

	table.sort(data, compare)

	return data
end


-- print the data in a formated matter to the screen
function print_hna(data)
	local value = data

	print("Announced network", "OLSR gateway", "Validity Time", "OLSR Hostname")

	for i, value in next, data do
		hna = string.format("%-18s", value["destination"] .. "/" .. value["genmask"])
		gw = string.format("%-15s" ,value["gateway"])
		vt = string.format("%8s", value["validityTime"])
		print(hna, gw, vt, value["hostname"])
	end
end


-- MAIN:
print("Fetching all HNAs may take some while!\n")

HNA_table = fetch_hna()
print_hna(HNA_table)
