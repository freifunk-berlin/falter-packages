'use strict';

const cfg = "qos";

let ffutils = require("freifunk.utils");
ffutils.guard(cfg);

ffutils.delete_section(cfg, "wan");
ffutils.commit(cfg);
