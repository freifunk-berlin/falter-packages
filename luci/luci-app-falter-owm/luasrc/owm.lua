--[[
LuCI - Lua Configuration Interface

Copyright 2013 Patrick Grimm <patrick@lunatiki.de>

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

$Id$

]]--

local bus = require "ubus"
local string = require "string"
local sys = require "luci.sys"
local uci = require "luci.model.uci".cursor_state()
local util = require "luci.util"
local json = require "luci.json"
local netm = require "luci.model.network"
local sysinfo = luci.util.ubus("system", "info") or { }
local boardinfo = luci.util.ubus("system", "board") or { }
local table = require "table"
local nixio = require "nixio"
local ip = require "luci.ip"

local ipairs, pairs, tonumber, tostring = ipairs, pairs, tonumber, tostring
local dofile, _G = dofile, _G

--- LuCI OWM-Library
-- @cstyle	instance
module "luci.owm"

-- backported from LuCI 0.11 and adapted form berlin-stats
--- Returns the system type (in a compatible way to LuCI 0.11)
-- @return	String indicating this as an deprecated value
--        	(instead of the Chipset-type)
-- @return	String containing hardware model information
--        	(trimmed to router-model only)
function sysinfo_for_kathleen020()
	local cpuinfo = nixio.fs.readfile("/proc/cpuinfo")

	local system = 'system is deprecated'

	local model =
		boardinfo['model'] or
		cpuinfo:match("machine\t+: ([^\n]+)") or
		cpuinfo:match("Hardware\t+: ([^\n]+)") or
		nixio.uname().machine or
		system

        return system, model
end

-- inspired by luci.version
--- Returns the system version info build from /etc/openwrt_release
--- switch from luci.version which always includes
--- the revision in the "distversion" field and gives empty "distname"
-- @ return	the releasename
--         	(DISTRIB_ID + DISTRIB_RELEASE)
-- @ return	the releaserevision
--         	(DISTRIB_REVISION)
function get_version()
	local distname = ""
	local distrel = ""
	local distrev = ""
	local version = {}

	dofile("/etc/openwrt_release")
	if _G.DISTRIB_ID then
		distname = _G.DISTRIB_ID
	end
	if _G.DISTRIB_RELEASE then
		distrel = _G.DISTRIB_RELEASE
	end
	if _G.DISTRIB_REVISION then
		distrev = _G.DISTRIB_REVISION
	end

        -- override with values from /etc/freifunk_release if possible          
        if nixio.fs.access("/etc/freifunk_release") then                        
                dofile("/etc/freifunk_release")                                 
                if _G.FREIFUNK_DISTRIB_ID then                                  
                        distname = _G.FREIFUNK_DISTRIB_ID             
                end                                                             
                if _G.FREIFUNK_RELEASE then                           
                        distrel = _G.FREIFUNK_RELEASE                      
                end                                                             
                if _G.FREIFUNK_REVISION then                                    
                        distrev = _G.FREIFUNK_REVISION                       
                end                                                             
        end                                                                     

	version['distname'] = distname .. " " .. distrel
	version['distrevision'] = distrev
	return version
end

function fetch_olsrd_config()
	local data = {}
	local IpVersion = uci:get_first("olsrd", "olsrd","IpVersion")
	if IpVersion == "4" or IpVersion == "6and4" then
		local jsonreq4 = util.exec("echo /config | nc 127.0.0.1 9090 2>/dev/null") or {}
		local jsondata4 = json.decode(jsonreq4) or {}
		if jsondata4['config'] then
			data['ipv4Config'] = jsondata4['config']
		end
	end
	if IpVersion == "6" or IpVersion == "6and4" then
		local jsonreq6 = util.exec("echo /config | nc ::1 9090 2>/dev/null") or {}
		local jsondata6 = json.decode(jsonreq6) or {}
		if jsondata6['config'] then
			data['ipv6Config'] = jsondata6['config']
		end
	end
	return data
end

function fetch_olsrd_links()
	local data = {}
	local IpVersion = uci:get_first("olsrd", "olsrd","IpVersion")
	if IpVersion == "4" or IpVersion == "6and4" then
		local jsonreq4 = util.exec("echo /links | nc 127.0.0.1 9090 2>/dev/null") or {}
		local jsondata4 = json.decode(jsonreq4) or {}
		local links = {}
		if jsondata4['links'] then
			links = jsondata4['links']
		end
		for i,v in ipairs(links) do
			links[i]['sourceAddr'] = v['localIP'] --owm sourceAddr
			links[i]['destAddr'] = v['remoteIP'] --owm destAddr
			local hostname = nixio.getnameinfo(v['remoteIP'], "inet")
			if hostname then
				links[i]['destNodeId'] = string.gsub(hostname, "mid..", "") --owm destNodeId
			end
		end
		data = links
	end
	if IpVersion == "6" or IpVersion == "6and4" then
		local jsonreq6 = util.exec("echo /links | nc ::1 9090 2>/dev/null") or {}
		local jsondata6 = json.decode(jsonreq6) or {}
		--print("fetch_olsrd_links v6 "..(jsondata6['links'] and #jsondata6['links'] or "err"))
		local links = {}
		if jsondata6['links'] then
			links = jsondata6['links']
		end
		for i,v in ipairs(links) do
			links[i]['sourceAddr'] = v['localIP']
			links[i]['destAddr'] = v['remoteIP']
			local hostname = nixio.getnameinfo(v['remoteIP'], "inet6")
			if hostname then
				links[i]['destNodeId'] = string.gsub(hostname, "mid..", "") --owm destNodeId
			end
			data[#data+1] = links[i]
		end
	end
	return data
end

function fetch_olsrd_neighbors(interfaces)
	local data = {}
	local IpVersion = uci:get_first("olsrd", "olsrd","IpVersion")
	if IpVersion == "4" or IpVersion == "6and4" then
		local jsonreq4 = util.exec("echo /links | nc 127.0.0.1 9090 2>/dev/null") or {}
		local jsondata4 = json.decode(jsonreq4) or {}
		--print("fetch_olsrd_neighbors v4 "..(jsondata4['links'] and #jsondata4['links'] or "err"))
		local links = {}
		if jsondata4['links'] then
			links = jsondata4['links']
		end
		for _,v in ipairs(links) do
			local hostname = nixio.getnameinfo(v['remoteIP'], "inet")
			if hostname then
				hostname = string.gsub(hostname, "mid..", "")
				local index = #data+1
				data[index] = {}
				data[index]['id'] = hostname --owm
				data[index]['quality'] = v['linkQuality'] --owm
				data[index]['sourceAddr4'] = v['localIP'] --owm
				data[index]['destAddr4'] = v['remoteIP'] --owm
				if #interfaces ~= 0 then
					for _,iface in ipairs(interfaces) do
						if iface['ipaddr'] == v['localIP'] then
							data[index]['interface'] = iface['name'] --owm
						end
					end
				end
				data[index]['olsr_ipv4'] = v
			end
		end
	end
	if IpVersion == "6" or IpVersion == "6and4" then
		local jsonreq6 = util.exec("echo /links | nc ::1 9090 2>/dev/null") or {}
		local jsondata6 = json.decode(jsonreq6) or {}
		local links = {}
		if jsondata6['links'] then
			links = jsondata6['links']
		end
		for _, link in ipairs(links) do
			local hostname = nixio.getnameinfo(link['remoteIP'], "inet6")
			if hostname then
				hostname = string.gsub(hostname, "mid..", "")
				local index = 0
				for i, v in ipairs(data) do
					if v.id == hostname then
						index = i
					end
				end
				if index == 0 then
					index = #data+1
					data[index] = {}
					data[index]['id'] = string.gsub(hostname, "mid..", "") --owm
					data[index]['quality'] = link['linkQuality'] --owm
					if #interfaces ~= 0 then
						for _,iface in ipairs(interfaces) do
							local name = iface['.name']
							local net = netm:get_network(name)
							local device = net and net:get_interface()
							if device and device:ip6addrs() then
								local local_ip = ip.IPv6(link.localIP)
								for _, a in ipairs(device:ip6addrs()) do
									if a:host() == local_ip:host() then
										data[index]['interface'] = name
									end
								end
							end
						end
					end
				end
				data[index]['sourceAddr6'] = link['localIP'] --owm
				data[index]['destAddr6'] = link['remoteIP'] --owm
				data[index]['olsr_ipv6'] = link
			end
		end
	end
	return data
end

function fetch_olsrd()
	local data = {}
	data['links'] = fetch_olsrd_links()
	local olsrconfig = fetch_olsrd_config()
	data['ipv4Config'] = olsrconfig['ipv4Config']
	data['ipv6Config'] = olsrconfig['ipv6Config']

	return data
end

function showmac(mac)
	if not is_admin then
		mac = mac:gsub("(%S%S:%S%S):%S%S:%S%S:(%S%S:%S%S)", "%1:XX:XX:%2")
	end
	return mac
end

function get_position()
	local position = {}
	uci:foreach("system", "system", function(s)
		position['latitude'] = tonumber(s.latitude)
		position['longitude'] = tonumber(s.longitude)
	end)
	if (position['latitude'] and  position['longitude']) then
		return position
	else
		return nil
	end
end

function get()
	local root = {}
	local ntm = netm.init()
	local position = get_position()
	local version = get_version()
	root.type = 'node' --owm
	root.updateInterval = 3600 --owm one hour

	root.system = {
		uptime = {sys.uptime()},
		loadavg = {sysinfo.load[1] / 65536.0},
		sysinfo = {sysinfo_for_kathleen020()},
	}

	root.hostname = sys.hostname() --owm
	root.hardware = boardinfo['system'] --owm

	root.firmware = {
		name=version.distname, --owm
		revision=version.distrevision --owm
	}

	root.freifunk = {}
	uci:foreach("freifunk", "public", function(s)
		local pname = s[".name"]
		s['.name'] = nil
		s['.anonymous'] = nil
		s['.type'] = nil
		s['.index'] = nil
		if s['mail'] then
			s['mail'] = string.gsub(s['mail'], "@", "./-\\.T.")
		end
		root.freifunk[pname] = s
	end)

	if position ~= nil then
		root.latitude = position["latitude"] --owm
		root.longitude = position["longitude"] --owm                                                        
	end                                                       
													
	root.links = fetch_olsrd_neighbors({})                                                                        
	root.olsr = fetch_olsrd()                                                                      
	root.script = 'luci-app-owm'                    
	root.api_rev = '1.0'

	return root
end
