'use strict';

import * as fs from 'fs';

let ffutils = require("freifunk.utils");

const import_dir = "/usr/share/ucode/freifunk/ffuplink";
const require_base = "freifunk.ffuplink.";

let files = fs.glob(import_dir + "/*.uc");

let uplink_mods = {};
let active = "";

for (let path in files) {
	let filename = fs.basename(path);
	let module = replace(filename, /\.uc$/, "");
	uplink_mods[module] = require(require_base + module);
}

function info() {
	let result = [];
	let current = get_current();
	for (let mod in uplink_mods) {
		let active = (mod == current) ? true : false;
		push(result, {
			mod:            mod,
			name:           uplink_mods[mod].name,
			description:    uplink_mods[mod].description,
			active:		active,
		});
	}
}

function get_current() {
	global.cursor.get("ffwizard", "settings", "ffuplink");
}

function configure(choice) {
	let current = get_current() || "nouplink";
	if (current == choice) return true;

	if (!uplink_mods[current].unconfigure()) return false;
	if (!uplink_mods[choice].configure()) return false;
	return global.cursor.set("ffwizard", "settings", "ffuplink", choice);
}

return {
	info,
	get_current,
	configure,
};
