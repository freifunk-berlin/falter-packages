'use strict';

const cfg = "luci";

let ffutils = require("freifunk.utils");
ffutils.guard(cfg);

const luci_defaults = {
	en:     "English",
};

ffutils.update_section(cfg, "languages", luci_defaults);
ffutils.commit(cfg);
