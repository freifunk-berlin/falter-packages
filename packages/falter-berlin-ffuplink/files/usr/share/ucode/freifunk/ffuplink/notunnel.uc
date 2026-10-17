'use strict';

//import * as rtnl from 'rtnl';

let ffutils = require("freifunk.utils");

//let ubus = require('ubus').connect();

const name = "notunnel";
const description = {
	en: {
		"DESCRIPTION": "Share internet directly over the WAN port",
	},
	de: {
		"DESCRIPTION": "notunnel Kartoffelbrei",
	},
};

function configure() {
	// set up the ffuplink interface
	let mac = global.cursor.get("network", "ffuplink_dev", "macaddr");
	if (!mac) mac = ffutils.generate_mac("fe");

	let ffuplink_interface = {
		proto:          "dhcp",
		hostname:	"ffuplink-" + replace(mac, ":", "") + "-uplink",
		disabled:       0,
        };

	let ffuplink_dev = {
		type:           "veth",
		peer_name:      "ffuplink_wan",
	};

	let result = ffutils.update_section("network", "ffuplink", ffuplink_interface);
	if (!result) return false;
	result = ffutils.update_section("network", "ffuplink_dev", ffuplink_dev);
	if (!result) return false;

	// set up br-wan
	global.cursor.foreach("network", "device", section => {
		if (section.name == "br-wan") {
			result = global.cursor.list_append("network", section[".name"], "ports", "ffuplink_wan");
		}
	});

	return result;
}

function unconfigure() {
	let result = false;
	// revert br-wan
	global.cursor.foreach("network", "device", section => {
		if (sectioni.name == "br-wan") {
			result = global.cursor.list_remove("network", section[".name"], "ports", "ffuplink_wan");
		}
	});
	if (!result) return false;

	let ffuplink_interface = {
		proto:          "none",
		hostname:	"",  // a delete
		disabled:       1,
        };

	let ffuplink_dev = {
		type:           "",  // a delete
		peer_name:      "",  // a delete
	};

	result = ffutils.update_section("network", "ffuplink", ffuplink_interface);
	if (!result) return false;
	result = ffutils.update_section("network", "ffuplink_dev", ffuplink_dev);

	// reload networking and force delete ffuplink
//	rtnl.request(rtnl.RTM_DELLINK || rtnl.const.RTM_DELLINK, 0, {dev: "ffuplink_wan"});
//	rtnl.request(rtnl.RTM_DELLINK || rtnl.const.RTM_DELLINK, 0, {dev: "veth0"});
//	const req = ubus.defer("network", "restart");
//	req.await();
	return result;
}

function status() {
	let iface = ffutils.cursor.get("network", "ffuplink", "disabled");
	let veth = ffutils.cursor.get("network", "ffuplink_dev", "type");
	if (iface == 0 && veth == "veth") return true;
	return false;
}

return {
	name: name,
	description: description,
	configure: configure,
	unconfigure: unconfigure,
	status: status,
};
