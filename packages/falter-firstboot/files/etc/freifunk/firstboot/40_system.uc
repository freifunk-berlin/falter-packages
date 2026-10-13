'use strict';

const cfg = "system";

let ffutils = require("freifunk.utils");
ffutils.guard(cfg);

const system_defaults = {
	hostname:       "gib-mir-einen-namen",
	cronloglevel:   10,
};

const ntp_defaults = {
	use_dhcp:       0,
	enable_server:  1,
};

if (global.cursor.get(cfg, "@system[0]", "hostname") == "OpenWrt")
	ffutils.update_section(cfg, "@system[0]", system_defaults);
ffutils.update_section(cfg, "ntp", ntp_defaults);
ffutils.commit(cfg);
