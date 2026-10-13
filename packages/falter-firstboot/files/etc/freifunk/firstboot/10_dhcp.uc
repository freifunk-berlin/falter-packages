'use strict';

const cfg = "dhcp";

let ffutils = require("freifunk.utils");
ffutils.guard(cfg);

const dnsmasq_defaults = {
	quietdhcp:      1,
	domain:         "ff",
	notinterface:   "wan",
	server:         [ "9.9.9.10", "194.150.168.168" ], // overwritten later by the wizard
};

const freifunk_domain_name = {
	name:           "frei.funk",
	ip:             "192.168.42.1",
};

// we need the first address in the ULA.  Drop off the /48 and replace with a 1
const ula = replace(global.cursor.get("network", "globals", "ula_prefix"), "/48", "1");
const freifunk_v6_domain_name = {
	name:           "frei.funk",
	ip:             ula,
};

const dhcp_dhcp = {
	interface:      "dhcp",
	start:          2,
	force:          1,
	ignore:         0,
	ra_default:     "",  // a delete
	ra_slaac:       "",  // a delete
	ra_flags:	"",  // a delete
	dhcp_option:    [ 3 ],
};

ffutils.update_section(cfg, "@dnsmasq[0]", dnsmasq_defaults);
ffutils.add_section(cfg, {type: "domain", name: "frei_funk"}, freifunk_domain_name);
ffutils.add_section(cfg, {type: "domain", name: "frei_funk_ipv6"}, freifunk_v6_domain_name);
global.cursor.rename(cfg, "lan", "dhcp");
ffutils.update_section(cfg, "dhcp", dhcp_dhcp);

ffutils.commit(cfg);
