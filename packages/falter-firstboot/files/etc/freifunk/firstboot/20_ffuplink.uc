'use strict';

const cfg = "ffuplink";
const network = "network";
const firewall = "firewall";

import * as fs from "fs";

let ffutils = require("freifunk.utils");
ffutils.guard(cfg);

// add the special routing table for ffuplink
let fd = fs.open("/etc/iproute2/rt_tables", "a");
if (!fd) return false;

fd.write("120 ffuplink\n");
fd.close();

// set up ffuplink's default state to "nouplink"
const ffuplink_iface_defaults = {
	device:         "ffuplink",
	proto:          "none",
	ip4table:       "ffuplink",
	ip6table:       "ffuplink",
	peerdns:        0,
	disabled:       1,
};

let mac = ffutils.generate_mac("fe"); // prefix mac with fe for "ffuplink egress"
const ffuplink_dev_defaults = {
	name:           "ffuplink",
	macaddr:        mac,
};

ffutils.add_section(network, {type: "interface", name: "ffuplink"}, ffuplink_iface_defaults);
ffutils.add_section(network, {type: "device", name: "ffuplink_dev"}, ffuplink_dev_defaults);

const zone_ffuplink = {
	name:           "ffuplink",
	input:          "REJECT",
	forward:        "ACCEPT",
	output:         "ACCEPT",
	network:        [ "ffuplink" ],
	masq:           1,
};

const forwarding_ffuplink = {
	dest:           "ffuplink",
	src:            "freifunk",
};

ffutils.add_section(firewall, {type: "zone", name: "zone_ffuplink"}, zone_ffuplink);
ffutils.add_section(firewall, {type: "forwarding", name: "fwd_ff_ffuplink"}, forwarding_ffuplink);

// set the ffwizard setting for ffuplink to nouplink
ffutils.update_section("ffwizard", "settings", {ffuplink: "nouplink"});


ffutils.commit(network);
ffutils.commit(firewall);
ffutils.commit("ffwizard");
