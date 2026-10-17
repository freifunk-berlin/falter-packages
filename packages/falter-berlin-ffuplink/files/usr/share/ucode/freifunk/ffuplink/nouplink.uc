'use strict';

let ffutils = require("freifunk.utils");

const name = "nouplink";
const description = {
	en: {
		"DESCRIPTION": "Do not share internet over the WAN port",
	},
	de: {
		"DESCRIPTION": "nouplink Kartoffelbrei",
	},
};

function configure() {
	// this is the default state and should always be set so
	// by the other modules when unconfigure() is called
	return true;
}

function unconfigure() {
	// this doesn't have any meaning as it's anyways unconfigured
	return true;
}

function status() {
	let iface = ffutils.cursor.get("network", "ffuplink", "disabled");
	if (iface == 1) return true;
	return false;
}

return {
	name: name,
	description: description,
	configure: configure,
	unconfigure: unconfigure,
	status: status,
};
