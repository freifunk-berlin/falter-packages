'use strict';

const cfg = "olsrd";
const network = "network";

let ffutils = require("freifunk.utils");
ffutils.guard(cfg);

const olsrd_plugins = [
	{
		library:        "olsrd_arprefresh",
	},
	{
		library:        "olsrd_nameservice",
		suffix:         ".ff",
		hosts_file:     "/tmp/hosts/olsr",
		latlon_file:    "/var/run/latlon.js",
		services_file:  "/var/etc/services.olsr",
	},
	{
		library:        "olsrd_jsoninfo",
		accept:         "0.0.0.0",
		ignore:         0,
	},
	{
		library:        "olsrd_dyn_gw",
		Ping:           [ "9.9.9.10", "194.150.168.168", "149.112.112.10" ],
		PingCmd:        "ping -c 1 -q -I ffuplink %s",
		PingInterval:   30,
		ignore:         0,
	},
];

// delete all the "LoadPlugin" and "Interface" sections, we start all over
global.cursor.foreach(cfg, "LoadPlugin", function(s) {
	global.cursor.delete(cfg, s[".name"]);
});
global.cursor.foreach(cfg, "Interface", function(s) {
	global.cursor.delete(cfg, s[".name"]);
});
for (let plugin_defaults in olsrd_plugins)
	ffutils.add_section(cfg, {type: "LoadPlugin"}, plugin_defaults);

const olsrd_defaults = {
	IpVersion:                      "4",
	FIBMetric:                      "flat",
	AllowNoInt:                     "yes",
	TcRedundancy:                   "2",
	NatThreshold:                   "0.75",
	LinkQualityAlgorithm:           "etx_ff",
	SmartGateway:                   "yes",
	SmartGatewayThreshold:          "50",
	Pollrate:                       "0.025",
	RtTable:                        "111",
	RtTableDefault:                 "112",
	RtTableTunnel:                  "113",
	RtTableTunnelPriority:          "100000",
	RtTableDefaultOlsrPriority:     "21000",
};

const olsrd_interface_defaults = {
	MidValidityTime:        "500.0",
	TcInterval:             "2.0",
	HnaValidityTime:        "125.0",
	HelloValidityTime:      "125.0",
	TcValidityTime:         "500.0",
	Ip4Broadcast:           "255.255.255.255",
	MidInterval:            "25.0",
	HelloInterval:          "3.0",
	HnaInterval:            "10.0",
};

ffutils.update_section(cfg, "@olsrd[0]", olsrd_defaults);
ffutils.add_section(cfg, {type: "InterfaceDefaults"}, olsrd_interface_defaults);
ffutils.commit(cfg);

const tunl0_defaults = {
	device:         "tunl0",
	proto:          "none",
};
ffutils.add_section(network, {type: "interface", name: "tunl0"}, tunl0_defaults);
ffutils.commit(network);

