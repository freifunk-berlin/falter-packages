'use strict';

const cfg = "uhttpd";

let ffutils = require("freifunk.utils");
ffutils.guard(cfg);

import { rand } from "math";

const uhttpd_main_defaults = {
	redirect_https:         1,
	rfc1918_filter:         0,
};

let commonname = "Freifunk Berlin - " + sprintf("%08x", rand(hex("ffffffff")));
const uhttpd_cert_defaults = {
	commonname:             commonname,
};

ffutils.update_section(cfg, "main", uhttpd_main_defaults);
ffutils.update_section(cfg, "defaults", uhttpd_cert_defaults);
ffutils.commit(cfg);
