'use strict';

let ffutils = require("freifunk.utils");

// handle setting up the qos config
function process(config) {
	if (!config || type(config) != "object") return false;

	// add a section for ffuplink rate limiting
	let download = config?.node?.internet?.download || "0";
	let upload = config?.node?.internet?.upload || "0";
	let section = {
		enabled:         1,
		download:        download,
		upload:          upload,
		classgroup:      "Default",
	};

	return ffutils.add_section("qos", {type: "interface", name: "ffuplink"}, section);
}

return {
	process,
};
