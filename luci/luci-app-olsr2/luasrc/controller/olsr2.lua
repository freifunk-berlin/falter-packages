module("luci.controller.olsr2", package.seeall)

local neigh_table = nil
local ifaddr_table = nil

function index()
	if not nixio.fs.access("/etc/config/olsrd2") then
		return
	end

	require("luci.model.uci")
	local uci = luci.model.uci.cursor_state()

	local page  = node("admin", "status", "olsr2")
	page.target = template("status-olsr2/overview")
	page.title  = _("OLSRv2")
	page.subindex = true
	page.acl_depends = { "luci-app-olsr2" }

	local page  = node("admin", "status", "olsr2", "json")
	page.target = call("action_json")
	page.title = nil
	page.leaf = true

	local page  = node("admin", "status", "olsr2", "neighbors")
	page.target = call("action_neigh")
	page.title  = _("Neighbors")
	page.subindex = true
	page.order  = 5

	--local page  = node("admin", "status", "olsr2", "interfaces")
	--page.target = call("action_interfaces")
	--page.title  = _("Interfaces")
	--page.order  = 10
end

function action_json()                                                             
        local http = require "luci.http"                                                             
        local utl = require "luci.util"                                  
        local uci = require "luci.model.uci".cursor()                                                                                  
        local json = require "luci.json"                                       
                                                                                                                                       
        local data, error = fetch_jsoninfo()                                       
        local jsonreq = json.encode(data)                                                            
        http.prepare_content("application/json")                                   
        http.write(jsonreq)                                               
end

local function local_mac_lookup(ipaddr)
	local _, rt
	for _, rt in ipairs(luci.ip.routes({ type = 1, src = ipaddr })) do
		local link = rt.dev and luci.ip.link(rt.dev)
		local mac = link and luci.ip.checkmac(link.mac)
		if mac then return mac end
	end
end

local function remote_mac_lookup(ipaddr)
	local _, n
	for _, n in ipairs(luci.ip.neighbors({ dest = ipaddr })) do
		local mac = luci.ip.checkmac(n.mac)
		if mac then return mac end
	end
end

-- Shamelessly stolen from https://stackoverflow.com/a/16643628
function get_ip_type(ip)
	local R = {IPV6 = 0, IPV4 = 1, ERROR = 2, STRING = 3}
	if type(ip) ~= "string" then return R.ERROR end

	-- check for format 1.11.111.111 for ipv4
	local chunks = {ip:match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")}
	if #chunks == 4 then
		for _,v in pairs(chunks) do
			if tonumber(v) > 255 then return R.STRING end
		end
		return R.IPV4
	end

	-- check for ipv6 format, should be 8 'chunks' of numbers/letters
	-- without leading/trailing chars
	-- or fewer than 8 chunks, but with only one `::` group
	local chunks = {ip:match("^"..(("([a-fA-F0-9]*):"):rep(8):gsub(":$","$")))}
	if #chunks == 8 or #chunks < 8 and ip:match('::') and not ip:gsub("::","",1):match('::') then
		for _,v in pairs(chunks) do
			if #v > 0 and tonumber(v, 16) > 65535 then
				return R.STRING
			end
		end
		return R.IPV6
	end

	return R.STRING
end

function action_neigh(json)
	local data, error = fetch_jsoninfo()
	local links = data['link']

	if error then
		return
	end

	local uci = require "luci.model.uci".cursor_state()
	local resolve = uci:get("luci_olsr2", "general", "resolve")
	local ntm = require "luci.model.network".init()
	local devices  = ntm:get_wifidevs()
	local sys = require "luci.sys"
	local assoclist = {}
	local ntm = require "luci.model.network"
	local ipc = require "luci.ip"
	local nxo = require "nixio"
	local defaultgw6
	local defaultgw4

	-- TODO: What if multiple default gateways exist with different metrics?
	-- make sure, that only the one with the highest priority gets selected.
	ipc.routes({ family = 4, type = 1, dest_exact = "0.0.0.0/0" },
		function(rt) defaultgw4 = rt.gw end)

	ipc.routes({ family = 6, type = 1, dest_exact = "::/0" },
		function(rt) defaultgw6 = rt.gw end)

	local function compare(a,b)
		local aproto = get_ip_type(a['neighbor_originator'])
		local bproto = get_ip_type(b['neighbor_originator'])

		if aproto == bproto then
			return a['domain_metric_in_raw']+a['domain_metric_out_raw'] < b['domain_metric_in_raw']+b['domain_metric_out_raw']
		else
			return aproto < bproto
		end
	end

	for _, dev in ipairs(devices) do
		for _, net in ipairs(dev:get_wifinets()) do
			local radio = net:get_device()
			assoclist[#assoclist+1] = {}
			assoclist[#assoclist]['ifname'] = net:ifname()
			assoclist[#assoclist]['network'] = net:network()[1]
			assoclist[#assoclist]['device'] = radio and radio:name() or nil
			assoclist[#assoclist]['list'] = net:assoclist()
		end
	end

	for k, v in ipairs(links) do
		local snr = 0
		local signal = 0
		local noise = 0
		local mac = ""
		local ip
		local neihgt = {}

		if resolve == "1" then
			hostname = nixio.getnameinfo(v['neighbor_originator'], nil, 100)
			if hostname then
				v['hostname'] = hostname
			end
		end

		local lmac = local_mac_lookup(v['link_bindto'])
		local rmac = remote_mac_lookup(v['neighbor_originator'])

		local proto = get_ip_type(v['neighbor_originator'])
		if proto == 0 then
			v['proto'] = 6
		elseif proto == 1 then
			v['proto'] = 4
		else
			v['proto'] = 0
		end

		for _, val in ipairs(assoclist) do
			if val.ifname == v['if'] and val.list then
				local assocmac, assot
				for assocmac, assot in pairs(val.list) do
					if rmac == luci.ip.checkmac(assocmac) then
						signal = tonumber(assot.signal)
						noise = tonumber(assot.noise)
						snr = (noise*-1) - (signal*-1)
					end
				end
			end
		end
		v['snr'] = snr
		v['signal'] = signal
		v['noise'] = noise
		if rmac then
			v['remote_mac'] = rmac
		end
		if lmac then
			v['local_mac'] = lmac
		end

		if defaultgw4 == v['neighbor_originator'] or defaultgw6 == v['neighbor_originator'] then
			v['defaultgw'] = 1
		end
	end

	table.sort(links, compare)
	luci.template.render("status-olsr2/neighbors", {links=links})
end

function action_interfaces()
	local data, has_v4, has_v6, error = fetch_jsoninfo('interfaces')
	local ntm = require "luci.model.network".init()

	if error then
		return
	end

	local function compare(a,b)
		return a.proto < b.proto
	end

	for k, v in ipairs(data) do
		local interface = ntm:get_status_by_address(v.olsrInterface.ipAddress)
		if interface then
			v.interface = interface
		end
	end

	table.sort(data, compare)
	luci.template.render("status-olsr/interfaces", {iface=data, has_v4=has_v4, has_v6=has_v6})
end

-- Internal
function fetch_jsoninfo()
	local uci = require "luci.model.uci".cursor_state()
	local utl = require "luci.util"
	local json = require "luci.json"
	
	local olsr2_port = tonumber(uci:get("olsrd2", "telnet", "port") or "") or 2009
	jsonreq_link = utl.exec("(echo '/nhdpinfo json link' | nc 127.0.0.1 %d) 2>/dev/null" % olsr2_port)
	
	if not jsonreq_link or jsonreq_link == "" then
		luci.template.render("status-olsr2/error_olsr")
		return nil, true
	end

	local jsondata = {}
	jsondata['link'] = json.decode(jsonreq_link)['link'] or {}

	return jsondata, false
end

