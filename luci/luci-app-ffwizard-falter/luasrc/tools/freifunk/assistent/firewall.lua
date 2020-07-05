local uci = require "luci.model.uci".cursor()
local tools = require "luci.tools.freifunk.assistent.tools"
local sharenet = uci:get("ffwizard","settings","sharenet")

module ("luci.tools.freifunk.assistent.firewall", package.seeall)

function prepareFirewall()
	local c = uci.cursor()
	local community = "profile"..c:get("freifunk","community","name")

	c:foreach("freifunk", "fw_forwarding", function(section)
		c:section("firewall", "forwarding", nil, section)
	end)
	c:foreach(community, "fw_forwarding", function(section)
		c:section("firewall", "forwarding", nil, section)
	end)

	c:foreach("freifunk", "fw_rule", function(section)
		c:section("firewall", "rule", nil, section)
	end)
	c:foreach(community, "fw_rule", function(section)
		c:section("firewall", "rule", nil, section)
	end)

	c:save("firewall")
end


function configureFirewall()
	tools.firewall_zone_add_interface("freifunk", "dhcp")
	uci:delete_all("firewall", "forwarding", {src="freifunk", dest="wan"})
	uci:save("firewall")
end
