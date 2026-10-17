'use strict';

import { access, popen, writefile } from "fs";
import { rand } from "math";

global.cursor = require("uci").cursor();

const CFG = "defaults";
const CFG_FILE = "/etc/config/" + CFG;
const SECTION_TYPE = "packages";
const SECTION_NAME = "default";

function guard(config) {
	// create the default config file if it doesn't yet exist
	let exists = access(CFG_FILE, "f");
	if (!exists) {
		writefile(CFG_FILE, "");
		global.cursor.set(CFG, SECTION_NAME, SECTION_TYPE);
	}

	// block future call the set the defaults
	if (!global.cursor.get(CFG, SECTION_NAME, config)) {
		global.cursor.set(CFG, SECTION_NAME, config, 1);
		global.cursor.commit(CFG);
	}
	// end the calling script now if defaults have already been set
	else {
		exit(0);
	}
}

function guard_rename(src, dest) {
	let current = global.cursor.get(CFG, SECTION_NAME, src);
	if (!current) return false;
	global.cursor.delete(CFG, SECTION_NAME, src);
	global.cursor.set(CFG, SECTION_NAME, dest, current);
	global.cursor.commit(CFG);
	return true;
}

function guard_delete(option) {
	let result = global.cursor.delete(CFG, SECTION_NAME, option);
	if (!result) return false;
	global.cursor.commit(CFG);
	return true;
}

function get_mac_label() {
	let fd = popen("(. /lib/functions/system.sh; get_mac_label)");
	if (!fd) return false;

	let addr = trim(fd.read('line'));
	fd.close();

	if (addr != "") return addr;

	let ubus = require("ubus").connect();
	let devstat = ubus.call("network.device", "status");
	let macs = [];
	for (let key in keys(devstat)) {
		if (devstat[key].macaddr != "00:00:00:00:00:00")
			push(macs, devstat[key].macaddr);
	}
	if (length(macs) == 0) return false;
	return sort(macs)[0];

}

function generate_hex_string(len, prefix) {
	// we assume any prefix is in the form XX:YY:ZZ
	let result = prefix ? prefix + ":" : "";
	let start = (length(result)+1)/3;
	for (let i = start; i < len ; i++) {
		result = result + sprintf("%02x", rand(hex("ff")+1)) + ":";
	}
	return substr(result, 0, -1); //drop the last ":"
}

function generate_mac(prefix) {
	let result = generate_hex_string(6, prefix);
	// ensure the MAC is unicast and locally administered
	let first_octet = hex(substr(result, 0, 2));
	first_octet = (first_octet | 0x02) & 0xFE;
	return sprintf("%02x", first_octet) + substr(result, 2);
}

function add_section(cfg, section, config) {
	let stype = section.type;
	let sname = section.name;

	if (stype == null) return false;

	if (sname == null) {
		sname = global.cursor.add(cfg, stype);
		if (!sname) return false;
	} else {
		let result = global.cursor.set(cfg, sname, stype);
		if (!result) return false;
	}

	for (let key, val in config)
		global.cursor.set(cfg, sname, key, val);
	return true;
}

function update_section(cfg, section, config) {
	let sname = "";

	if (type(section) == "string" ) {
		sname = section;
	}
	// find the anonymouns section, if the section is an object
	// expected format is { type: "type of section"; key: "value" }
	// where type is the section type of the anonymous section and
	// key/value are the options in the section to match against
	else if (type(section) == "object") {
		let type = section.type;
		let key = keys(section)[1];
		let val = section[key];
		global.cursor.foreach(cfg, type, function(s) {
			if (s[key] == val)
				sname = s[".name"];
		});
	}

	// return false on an empty section name or if it doesn't exist
	if (sname == "") return false;
	if (global.cursor.get(cfg, sname) == null) return false;

	for (let key, val in config)
		global.cursor.set(cfg, sname, key, val);
	return true;
}

function delete_section(cfg, section) {
	let sname = "";

	if (type(section) == "string") {
		sname = section;
	}
	else if (type(section) == "object") {
		let type = section.type;
		let key = keys(section)[1];
		let val = section[key];
		global.cursor.foreach(cfg, type, function(s) {
			if (s[key] == val)
				sname = s[".name"];
		});
	}

	if (sname == "") return false;
	return global.cursor.delete(cfg, sname);
}

function commit(cfg) {
	return global.cursor.commit(cfg);
}

return {
	guard,
	guard_rename,
	guard_delete,
	generate_hex_string,
	generate_mac,
	get_mac_label,
	add_section,
	update_section,
	delete_section,
	commit,
};
