import { cursor } from "uci";

const version = ubus.call("file", "read", {path: "/etc/freifunk_release",});

function getvalue(p) {
	if (!version) { return ""; }

	for (let line in split(version.data, "\n")) {
		let m = match(line, /^([A-Z0-9_]+)=['"]?([^'"]+)['"]?$/);
		if (m && m[1] == p) {
			return m[2];
		}
	}
};

const uci = cursor();

gauge("node_freifunk_info")({
	distrib_id: getvalue("FREIFUNK_DISTRIB_ID"),
	release: getvalue("FREIFUNK_RELEASE"),
	revision: getvalue("FREIFUNK_REVISION"),
	variant: getvalue("FREIFUNK_VARIANT"),
	community: uci.get("freifunk", "community", "name"),
	latitude: uci.get("system", "@system[0]", "latitude"),
	longitude: uci.get("system", "@system[0]", "longitude"),
}, 1);
