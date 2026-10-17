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

// set the ip address of the dhcp interface.
function dhcp(config) {
	if (!config || type(config) != "string") return false;

	const update = {
		ipaddr:         first_ip(config),
	};

	return ffutils.update_section("network", "dhcp", update);
}

// Add the wirelessN mesh interfaces
function mesh(config, index) {
	if (!config || type(config) != "string" || index == null) return false;

	const section = {
		proto:          "static",
		ipaddr:         [ first_ip(config) ],
		ip6assign:      64,
	};
	const iface = "wireless" + sprintf("%d", index);

	return ffutils.add_section("network", {type: "interface", name: iface}, section);
}

// Add the dhcp and wirelessN interfaces to the network config
function process(config) {
	if (!config || type(config) != "object") return false;

	let result = dhcp(config?.node?.ips?.dhcp);
	if (!result) return false;

	let idx = 0;
	global.cursor.foreach("wireless", "wifi-device", function(s) {
		result = mesh(config?.node?.ips?.mesh?.[idx], idx);
		if (!result) return false;
		idx++;
	});

	return true;
}

return {
	process,
};
