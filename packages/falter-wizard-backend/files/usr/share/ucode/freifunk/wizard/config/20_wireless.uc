'use strict';

let ffutils = require("freifunk.utils");
let community = require("freifunk.community");

function process(config) {
	if (!config || type(config) != "object") return false;
	let dev_defaults = community.get_defaults("wireless", "wifi-device");

	global.cursor.foreach("wireless", "wifi-device", function(s) {
		let device = s[".name"];
		let band = s.band;
		let channel = dev_defaults[band]?.channel;

		// device settings
		let section = dev_defaults[band];
		let ch_override = config?.node?.wireless?.channel_overridei?.[band];
		if (ch_override) {
			channel = ch_override;
			section["channel"] = ch_override;
		}
		let result = ffutils.update_section("wireless", device, section);
		if (!result) return false;

		// add mesh interface
		let meshmode = config?.node?.wireless?.[band]?.meshmode || "mesh";
                let mesh_defaults = community.get_defaults("wireless", meshmode);
		section = mesh_defaults[band];
		section["device"] = device;
		// set the ssid, bssid or any other channel specific attributes
		// this is genrally only used for ad-hoc
		let channel_str = sprintf("%d", channel);
		let channel_opts = mesh_defaults?.channel?.[channel_str];
		if (channel_opts) {
			for (let key in keys(channel_opts)) {
				section[key] = channel_opts[key];
			}
		}
		// generate the ifname and section name
		let ifname = "wlan" + substr(device, -1, 1) + "-" + meshmode + "-" + substr(band, 0, 1);
		let section_name = replace(ifname, "-", "_");
		section["ifname"] = ifname;
		// set the network name "wirelessN"
		section["network"] = "wireless" + substr(device, -1, 1);
		result = ffutils.add_section("wireless", {type: "wifi-iface", name: section_name,}, section);
		if (!result) return false;

		// set the ssid for the dhcp wifi-iface
		let ssid = config?.node?.wireless?.ssid;
		section_name = "wlan" + substr(device, -1, 1) + "_dhcp_" + substr(band, 0, 1);
		result = global.cursor.set("wireless", section_name, "ssid", ssid);
		if (!result) return false;
	});
	return true;
}

return {
	process,
};
