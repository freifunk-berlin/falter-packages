'use strict';

const cfg = "ffwizard";

import { access, writefile } from "fs";

let ffutils = require("freifunk.utils");
ffutils.guard(cfg);

// ensure the config file exists with a "settings" section
let exists = access("/etc/config/ffwizard", "f");
if (!exists) {
	writefile("/etc/config/ffwizard", "");
	global.cursor.set(cfg, "settings", "settings");
}

ffutils.commit(cfg);
