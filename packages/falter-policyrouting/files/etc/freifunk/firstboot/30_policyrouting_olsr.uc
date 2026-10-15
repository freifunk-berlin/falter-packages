'use strict';

const cfg = "policyrouting-olsr";

import * as fs from "fs";

let ffutils = require("freifunk.utils");
ffutils.guard(cfg);

// insert the OLSRd routing tables
let fd = fs.open("/etc/iproute2/rt_tables", "a");
if (!fd) return false;

fd.write("110 localnets\n111 olsr\n112 olsr-default\n113 olsr-tunnel\n");
fd.close();
