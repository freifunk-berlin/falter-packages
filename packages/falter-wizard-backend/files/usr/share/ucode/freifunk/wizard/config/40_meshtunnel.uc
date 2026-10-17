'use strict';

let ffutils = require("freifunk.utils");
let community = require("freifunk.community");

function process(config) {
	if (!config || type(config) != "object" )  return false;

	let meshtunnel = config?.node?.internet?.meshtunnel;
	if (meshtunnel == false) return true;

	// add meshtunnel interface and device to network
	let interface = {
		proto:         "dhcp",
		device:        meshtunnel,
		disabled:      0,
	};
	let mac = ffutils.generate_mac("b6");
	let device = {
		macaddr:       mac,
		name:          meshtunnel,
	};
	let result = ffutils.add_section("network", {type: "interface", name: meshtunnel}, interface);
	if (!result) return false;
	result = ffutils.add_section("network", {type: "device", name: meshtunnel + "_dev"}, device);
	if (!result) return false;

	// add the meshtunnel interface to freifunk zone
	result = global.cursor.list_append("firewall", "zone_freifunk", "network", meshtunnel);
	if (!result) return false;

	// add the meshtunnel to olsr
	interface = {
		interface:      meshtunnel,
		Mode:          "ether",
		ignore:        0,
	};
	result = ffutils.add_section("olsrd", {type: "Interface", name: meshtunnel,}, interface);
	if (!result) return false;

	// add to tunneldigger
	let td_config = community.get_defaults("tunneldigger", meshtunnel);
	td_config["uuid"] = ffutils.generate_hex_string(10, mac);
	return ffutils.add_section("tunneldigger", {type: "broker", name: meshtunnel,}, td_config);
}

return {
	process,
};
