'use strict';

let ffutils = require("freifunk.utils");
let community = require("freifunk.community");

function calculate_limit(net) {
	if (!net || type(net) != "string") return false;

	let parts = split(net, "/");
	if (length(parts) != 2) return false;

	let cidr = int(parts[1]);
	if (!cidr || cidr > 32 || cidr < 8) return false;

	return (2 ** (32 - cidr)) - 2;
}

function dhcp(config) {
	if (!config || type(config) != "string") return false;

	let limit = calculate_limit(config);
	if (!limit) return false;

	let section = community.get_defaults("dhcp", "dhcp");
	if (!config) return false;

	section["limit"] = limit;
	return ffutils.update_section("dhcp", "dhcp", section);
}

function process(config) {
	if (!config || type(config) != "object") return false;

	// set up the dhcp interface
	if (!dhcp(config?.node?.ips?.dhcp)) return false;

	// set up the general dnsmasq section
	let section = community.get_defaults("dhcp", "@dnsmasq[0]");
	return ffutils.update_section("dhcp", "@dnsmasq[0]", section); 
}

return {
	process,
};
