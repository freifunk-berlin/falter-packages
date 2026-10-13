'use strict';

const cfg = "firewall";

let ffutils = require("freifunk.utils");
ffutils.guard(cfg);

const defaults = {
	input:                  "ACCEPT",
	drop_invalid:           0,
	syn_flood:              "",  // a delete
	synflood_protect:       1,
};

const zone_wan = {
	input:          "ACCEPT",
	forward:        "REJECT",
	network:        [ "wan" ],
	mtu_fix:        "",  // a delete
};

const zone_freifunk = {
	name:           "freifunk",
	input:          "ACCEPT",
	output:         "ACCEPT",
	forward:        "ACCEPT",
	network:        [ "dhcp", "tunl0" ],
	device:         [ "tnl_+" ],
};

const rules_freifunk = [
	{
		proto:          "icmp",
		target:         "ACCEPT",
		src:            "freifunk",
	},
	{
		proto:          "tcp",
		target:         "ACCEPT",
		src:            "freifunk",
		dest_port:      "22 80 443",
	},
];

const forwarding_freifunk = {
	src:            "wan",
	dest:           "freifunk",
};

const lan_to_dhcp = {
	dest:            "dhcp",
};

ffutils.update_section(cfg, "@defaults[0]", defaults);
ffutils.update_section(cfg, {type: "zone", name: "wan"}, zone_wan);
ffutils.delete_section(cfg, {type: "zone", name: "lan"});
ffutils.delete_section(cfg, {type: "forwarding", src: "lan"});
ffutils.add_section(cfg, {type: "zone", name: "zone_freifunk"}, zone_freifunk);
for (let i = 0; i < length(rules_freifunk); i++)
	ffutils.add_section(cfg, {type: "rule"}, rules_freifunk[i]);
ffutils.add_section(cfg, {type: "forwarding"}, forwarding_freifunk);
ffutils.update_section(cfg, {type: "rule", name: "Allow-IPSec-ESP"}, lan_to_dhcp);
ffutils.update_section(cfg, {type: "rule", name: "Allow-ISAKMP"}, lan_to_dhcp);


ffutils.commit(cfg);
