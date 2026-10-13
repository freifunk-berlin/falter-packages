'use strict';

const cfg = "pingcheck";

let ffutils = require("freifunk.utils");
ffutils.guard(cfg);

const pingcheck_defaults = {
	host:           "livecheck.berlin.freifunk.net",
	timeout:        60,
};

ffutils.update_section(cfg, "@default[0]", pingcheck_defaults);
ffutils.delete_section(cfg, {type: "interface", name: "sta"});
ffutils.commit(cfg);
