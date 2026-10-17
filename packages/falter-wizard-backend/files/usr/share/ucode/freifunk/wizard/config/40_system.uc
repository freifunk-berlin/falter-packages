'use strict';

let ffutils = require("freifunk.utils");
let community = require("freifunk.community");

// handle setting up the system config
function process(config) {
	if (!config || type(config) != "object") return false;

	// handle @system[0]
	let section = community.get_defaults("system", "@system[0]");
	let hostname = config?.node?.hostname;
	if (!hostname) return false;
	let lat = config?.node?.latitude || "";
	let lon = config?.node?.longitude || "";
	let alt = config?.node?.altitude || "";
	let location = config?.node?.location || "";
	section["hostname"] = hostname;
	section["latitude"] = lat;
	section["longitude"] = lon;
	section["altitude"] = alt;
	section["location"] = location;

	return ffutils.update_section("system", "@system[0]", section);
}

return {
	process,
};
