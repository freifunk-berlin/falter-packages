'use strict';

import { popen } from "fs";

let fd = popen("df");
while (true) {
	if (!fd) break;

	let line = fd.read('line');
	if (!line) break;

	let m = match(trim(line), /(\d+)\s+(\d+)\s+(\d+)\s+(\d+)%\s+\/$/);
	if (!m) continue;

	let size = m[1] * 1024;
	let used = m[2] * 1024;
	gauge("filesystem_size")(null, size);
	gauge("filesystem_used")(null, used);
}

