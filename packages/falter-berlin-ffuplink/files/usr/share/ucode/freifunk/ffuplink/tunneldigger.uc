'use strict';

//import * as rtnl from 'rtnl';

let ffutils = require("freifunk.utils");
let community = require("freifunk.community");

//let ubus = require('ubus').connect();

const name = "notunnel";
const description = {
	en: {
		"DESCRIPTION": "Share internet through the Freifunk VPN tunnel",
	},
	de: {
		"DESCRIPTION": "tunneldigger Kartoffelbrei",
	},
};

function configure() {
	// set up the ffuplink interface
	let ffuplink_interface = {
		proto:          "dhcp",
		disabled:       0,
        };
	let result = ffutils.update_section("network", "ffuplink", ffuplink_interface);
	if (!result) return false;

	// set up tunneldigger
	let td_config = community.get_defaults("tunneldigger", "ffuplink");
	let mac = global.cursor.get("network", "ffuplink_dev", "macaddr");
	if (!mac) mac = ffutils.generate_mac("fe");

	td_config["uuid"] = ffutils.generate_hex_string(10, mac);
	result = ffutils.add_section("tunneldigger", {type: "broker", name: "ffuplink",}, td_config);

	return result;
}

function unconfigure() {
	let ffuplink_interface = {
		proto:          "none",
		disabled:       1,
        };
	let result = ffutils.update_section("network", "ffuplink", ffuplink_interface);
	if (!result) return false;
	result = ffutils.delete_section("tunneldigger", "ffuplink");

	return result;
}

function status() {
	let iface = ffutils.cursor.get("network", "ffuplink", "disabled");
	let td_status = ffutils.cursor.get("tunneldigger", "ffuplink", "enabled");
	if (iface == 0 && td_status == 1) return true;
	return false;
}

return {
	name: name,
	description: description,
	configure: configure,
	unconfigure: unconfigure,
	status: status,
};
