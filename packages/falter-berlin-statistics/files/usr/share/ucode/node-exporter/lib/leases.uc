import { glob, readfile } from "fs";

function ip2int(ip) {
	let b = iptoarr(ip);
	if (length(b) != 4) return null;

	return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
}

function contains(min, max, ip) {
	return (ip2int(ip) >= ip2int(min)) && (ip2int(ip) <= ip2int(max));
}

function get_networks() {
	let result = {};
	let files = glob("/tmp/etc/dnsmasq.conf*");
	for (let path in files) {
		let data = split(readfile(path), "\n");
		for (let line in data) {
				let m = match(line, /^dhcp-range=set:([^,]+),([0-9.]*),([0-9.]*)/);
				if (m)
					result[m[1]] = {
						network: m[1],
						min:     m[2],
						max:     m[3],
						size:    ip2int(m[3]) - ip2int(m[2]) + 1,
						count:   0,
					};
		}
	}

	return result;

}

let count = 0;
function get_leases() {
	let result = [];
	let data = split(trim(readfile("/tmp/dhcp.leases")), "\n");
	for (let line in data) {
		let fields = split(line, " ");
		push(result, fields[2]);
		count++;
	}
	return result;
}

let networks = get_networks();
let leases = get_leases();
for (let lease in leases) {
	for (let n in networks) {
		if (contains(networks[n].min, networks[n].max, lease)) {
			networks[n].count++;
			continue;
		}
	}
}

let network_count = gauge("network_leases_count");
let network_percent = gauge("network_leases_percent");
for (let n in networks) {
	network_count( {network: n}, networks[n].count);
	network_percent( {network: n}, (1.0 * networks[n].count) / networks[n].size);
}

counter("node_dhcpleases_leases")(null, count);
