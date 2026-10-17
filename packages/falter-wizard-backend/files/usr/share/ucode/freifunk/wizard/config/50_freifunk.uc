'use strict';

let ffutils = require("freifunk.utils");
let community = require("freifunk.community");

// handle setting up the freifunk config
function process(config) {
	if (!config || type(config) != "object") return false;

	// handle the contact section
	let section = config?.contact || {};
	// the location is also part of the contact section
	// the location is also in system.@system[0].location
	let location = config?.node?.location || "";
	section["location"] = location;

	let result = ffutils.update_section("freifunk", "contact", section);
	if (!result) return false;

	// handle the community section
	section = community.get_defaults("freifunk", "community");
	// the latatude and longitue are also in system.@system[0]
	let lat = config?.node?.latitude || "";
	let lon = config?.node?.longitude || "";
	section["latitude"] = lat;
	section["longitude"] = lon;
	return ffutils.update_section("freifunk", "community", section);
}

return {
	process,
};
