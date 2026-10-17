'use strict';

let ffutils = require("freifunk.utils");

// Add the wirelessN interfaces to the freifunk zone.  The dhcp interface and 
// any mesh-specific interface are already added by firstboot
function process(config) {
	if (!config || type(config) != "object") return false;

	let network = global.cursor.get("firewall", "zone_freifunk", "network");
	if (!network) return false;

	let index = 0;
	global.cursor.foreach("wireless", "wifi-device", function(s) {
		push(network, "wireless" + index);
		index++;
	});

	return global.cursor.set("firewall", "zone_freifunk", "network", network);
}

return {
	process,
};
