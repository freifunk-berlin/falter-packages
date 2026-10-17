'use strict';

let ffutils = require("freifunk.utils");

function first_ip(net) {
	if (!net || type(net) != "string") return false;

	let parts = split(net, "/");
	if (length(parts) != 2) return false;

	let addr = iptoarr(parts[0]);
	if (!addr) return false;

	let cidr = int(parts[1]);
	if (!cidr || cidr > 32 || cidr < 8) return false;

	if (cidr <= 30) addr[3]++;

	return sprintf("%s/%d", arrtoip(addr), cidr);
}

// handle setting up the prometheus-push config
function process(config) {
	if (!config || type(config) != "object") return false;

	let addr = first_ip(config?.node?.ips?.dhcp);
	let section = {
		interface:       split(addr, "/")[0], // from cidr from addr
	};

	return ffutils.update_section("prometheus-push", "ping", section);
}

return {
	process,
};

