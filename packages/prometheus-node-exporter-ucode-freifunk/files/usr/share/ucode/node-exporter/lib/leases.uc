const x = ubus.call("file", "read", {path: "/tmp/dhcp.leases",});

if (!x) {
	counter("node_dhcpleases_leases")(null, 0);
}
else {
	let count = length(split(x.data, "\n"));
	counter("node_dhcpleases_leases")(null, count-1);
}
