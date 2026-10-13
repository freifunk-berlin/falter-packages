'use strict';

const cfg = "freifunk";

let ffutils = require("freifunk.utils");
ffutils.guard(cfg);

// set the default community to berlin
const community_defaults = {
	name:           "berlin",
	homepage:       "https://berlin.freifunk.net",
};

ffutils.update_section(cfg, "community", community_defaults);
ffutils.commit(cfg);
