'use strict';

const cfg = "wireless";

import { readfile } from "fs";
let ffutils = require("freifunk.utils");
ffutils.guard(cfg);

const board = json(readfile("/etc/board.json"));
if (!board.wlan) return;

// find out the best ht mode, 20mhz for 2g, 40mhz for 5g, and 80mhz for 6g
let modes = {};
for (let phy in board.wlan) {
	let band = keys(board.wlan[phy].info.bands)[-1];
	let modelist = board.wlan[phy].info.bands[band].modes;
	if (band == "2G") {
		for (let mode in reverse(modelist)) {
			if (match(mode, /20$/)) {
				modes["2g"] = mode;
				break;
			}
		}
	} else if (band == "5G") {
		for (let mode in reverse(modelist)) {
			if (match(mode, /40$/)) {
				modes["5g"] = mode;
				break;
			}
		}
	} else if (band == "6G") {
		for (let mode in reverse(modelist)) {
			if (match(mode, /80$/)) {
				modes["6g"] = mode;
				break;
			}
		}
	}
}

const wifi_device = {
	"2g": {
		htmode:         modes["2g"] || "HT20",
                country:        "DE",
		channel:        13,
		doth:           0,
		diversity:      1,
		distance:       1000,
		disabled:       0,
	},
	"5g": {
		htmode:         modes["5g"] || "HT40",
		country:        "DE",
		doth:           0,
		disabled:       0,
	},
	"6g": {
		htmode:         modes["6g"] || "HT80",
		country:        "DE",
		doth:           0,
		disabled:	0,
	},
};

let device_map = {}; // associate "radioN" with "Yg"
global.cursor.foreach(cfg, "wifi-device", function(s) {
	ffutils.update_section(cfg, s[".name"], wifi_device[s.band]);
	device_map[s[".name"]] = s.band;
});

// determine how to name the setup ssid
let ssidlabel = ffutils.get_mac_label();
if (!ssidlabel) {
	ssidlabel = substr(board.model.id, 0, 16);
} else {
	// just the last 3 octets of the mac
	ssidlabel = substr(ssidlabel, 9);
}
ssidlabel = trim("freifunk-setup " + ssidlabel);

// create the wifi-iface firstboot settings
let wifi_iface = {};
for (let device in keys(device_map)) {
	let ifname = "wlan" + substr(device, -1, 1) + "-dhcp-" + substr(device_map[device], 0, 1);
	wifi_iface[device_map[device]] = {
		ifname:         ifname,
		ssid:           ssidlabel,
		network:        "dhcp",
		disabled:       0,
	};
}

global.cursor.foreach(cfg, "wifi-iface", function(s) {
	ffutils.update_section(cfg, s[".name"], wifi_iface[device_map[s.device]]);
	global.cursor.rename(cfg, s[".name"], replace(wifi_iface[device_map[s.device]].ifname, "-", "_"));
});

ffutils.commit(cfg);
