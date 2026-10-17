'use strict';

let ffutils = require("freifunk.utils");
let ffuplink = require("freifunk.ffuplink");

// configure the selected ffuplink type
function process(config) {
	if (!config || type(config) != "object" || !index) return false;

	let choice = config?.node?.internet?.ffuplink;
	if (!choice) return false;

	return ffuplink.configure(choice);
}

return {
	process,
};
