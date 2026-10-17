'use strict';

let ffutils = require("freifunk.utils");

const cfg = "ffwizard";

function process(config) {
	if (!config || type(config) != "object") return false;

	// ffuplink is set already in the ffuplink module
	const settings = {
		runbefore:            "true",
		meshtunnel:           config?.node?.internet?.meshtunnel,
		statistics:           config?.node?.statistics ? "1" : "0",
		upload:               config?.node?.internet?.upload,
		download:             config?.node?.internet?.download,
		ssid:                 config?.node?.wireless?.ssid,
	};
	let result = ffutils.update_section(cfg, "settings", settings);
	if (!result) return false;

	const ips = {
		dhcp:                 config?.node?.ips?.dhcp,
		mesh:                 config?.node?.ips?.mesh,
		ipv6:                 config?.node?.ips?.ipv6,
	};
	result = ffutils.add_section(cfg, {type: "ips", name: "ips"}, ips);
	if (!result) return false;

	for (let key in keys(config?.node?.wireless)) {
		if (match(key, /^(\d+\D)$/)) {
			let wireless = {
				meshmode:             config.node.wireless[key].meshmode,
				channel_override:     config.node.wireless[key]?.channel_override || "",
			};
			result = ffutils.add_section(cfg, {type: "wireless", name: key}, wireless);
			if (!result) return false;
		}
	}
	return true;
}

return {
	process,
};
