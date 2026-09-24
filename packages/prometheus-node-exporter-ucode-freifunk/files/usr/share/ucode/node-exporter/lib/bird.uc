import { popen } from 'fs';
import { cursor } from 'uci';
let resolv = require('resolv');

function exec(cmd) {
	let fh = popen(cmd, 'r');
	if (fh) { let r = trim(fh.read('all')); fh.close(); return r; }
	return '';
}

function build_neigh_tables() {
	let ipv6 = exec('ip -j -6 neigh show 2>/dev/null');
	if (!ipv6 || length(ipv6) == 0) return [];
	let entries6 = json(ipv6);
	if (!entries6) return [];

	let cleaned = filter(entries6, (val) => index(val.dst, "fe80") == 0);

	let ipv4 = exec('ip -j -4 neigh show 2>/dev/null');
	if (!ipv4) return [];
	let entries4 = json(ipv4);
	if (!entries4) return [];

	for (let i = 0; i < length(entries4); i++) {
		for (let j = 0; j < length(cleaned); j++) {
			if ( cleaned[j].lladdr == entries4[i].lladdr ) {
				cleaned[j].ip4 = entries4[i].dst;
			}
		}
	}

	// add any gre tunnels
	let gre = popen('ip tunnel show');
	while (true) {
		if (!gre) return cleaned;

		let line = gre.read('line');
		if (!line) break;

		if (index(line, "gre4-") == 0) {
			let data = wsplit(line);
			push(cleaned, {
				dst: substr(data[0], 0, -1),
				ip4: data[3],
			});
		}
	}
	gre.close();

	return cleaned;
}

function get_ts_wgN_ip(iface) {
	let wg = poneline("wg show " + iface + " endpoints");
	if (!wg) {
		return "fe80::1";
	}
	let data = wsplit(wg);
	let result = trim(split(data[1], /[\s:]+/)[0]);
	return result
}

function resolve_hostname(ip) {
	if ( iptoarr(ip) == null ) return ip;
	let result = resolv.query(ip, { type: ['PTR'] });
	for (let domain in result) {
		let ptr = result[domain]?.PTR?.[0];
		if (!ptr) continue;
		let m = match(ptr, /([^.]+)\.ff$/);
		if (m) return m[1];
	}
	return ip;
}

let tables = build_neigh_tables();

const uci = cursor();
const thishost = uci.get("system", "@system[0]", "hostname");

// birdc show babel routes

let babel_ipv4_gateway_metric = gauge("babel_ipv4_gateway_metric");
let babel_ipv4_gateway_selection = gauge("babel_ipv4_gateway_selection");
let gw4_done = [];
let babel_ipv6_gateway_metric = gauge("babel_ipv6_gateway_metric");
let babel_ipv6_gateway_selection = gauge("babel_ipv6_gateway_selection");
let gw6_done = [];

let fd = popen("birdc show babel routes");
while(true) {
	if (!fd) break;

	let line = fd.read('line');
	if (!line) break;

	if (index(line, "0.0.0.0/0") == 0) {
		let data = wsplit(line);
		let ip = data[1];

		if (index(data[2], "wg_") == 0 ) {
			// add more info to the neighbor table
			push(tables, {dst: data[2], ip4: data[1],});
		}

		if (index(data[2], "gre4-") == 0 ){
			ip = data[2];
			let lookup = filter(tables, function(row) {
				return row.dst == data[2];
			});
			if (lookup[0]) {
				ip = lookup[0].ip4;
			}
			let x = resolve_hostname(ip);
		}

		let hostname = resolve_hostname(ip);
		if (hostname == thishost) continue;
		if (index(gw4_done, hostname + data[2]) != -1) continue;

		push(gw4_done, hostname + data[2]);

		babel_ipv4_gateway_metric( {host: hostname, iface: data[2],}, data[3]);
		let selected = -1;
		if (data[4] == "*") {
			selected = 1;
		}
		if (data[4] == "+") {
			selected = 0;
		}
		babel_ipv4_gateway_selection( {host: hostname, iface: data[2],}, selected);
	}
	else if (index(line, "::/0") == 0) {
		let data = wsplit(line);
		let key = data[3];
		let hostname = data[3];

		if (index(data[4], "wg_") == 0 || index(data[4], "gre4-") == 0) {
			key = data[4];
			hostname = data[4];
		}

		let lookup = filter(tables, function(row) {
			return row.dst == key;
		});

		if (lookup[0]) {
			hostname = resolve_hostname(lookup[0].ip4);
		}
		else {
			// this could be a ts_wgN interface
			let ip = get_ts_wgN_ip(data[4]);
			if (ip == data[4]) {
				// sometimes we need to repeat this
				ip = get_ts_wgN_ip(data[4]);
			}
			hostname = resolve_hostname(ip);
		}
		if (hostname == thishost) continue;
		if (index(gw6_done, hostname + data[4]) != -1) continue;

		push(gw6_done, hostname + data[4]);

		babel_ipv6_gateway_metric( { host: hostname, iface: data[4],}, data[5]);
		let selected = -1;
		if (data[6] == "*") {
			selected = 1;
		}
		if (data[6] == "+") {
			selected = 0;
		}
		babel_ipv6_gateway_selection( {host: hostname, iface: data[4],}, selected)
	}
}
fd.close();


// birdc show babel neighbors

let babelneigh_rtt = gauge("babel_neighbor_rtt");
let babelneigh_metric = gauge("babel_neighbor_metric");
let babelneigh_done = [];

fd = popen("birdc show babel neighbors");
while (true) {
	if (!fd) break;

	let line = fd.read('line');
	if (!line) break;

	let data = wsplit(line);
	let key = data[0];
	let hostname = data[0];

	if (index(data[0], "fe80") == 0) {
		if(index(data[1], "wg_") == 0 || index(data[1], "gre4-") == 0) {
			key = data[1];
			hostname = data[1];
		}

		let lookup = filter(tables, function(row) {
			return row.dst == key;
		});
		if (lookup[0]) {
			hostname = resolve_hostname(lookup[0].ip4);
		}
		else if (index(data[1], "ts_wg") == 0) {
			// this could be a ts_wgN interface
			let ip = get_ts_wgN_ip(data[1]);
			if (ip == data[1]) {
				// sometimes we need to repeat this
				ip = get_ts_wgN_ip(data[1]);
			}
			hostname = resolve_hostname(ip);
		}

		if (hostname == thishost) continue;
		if (index(babelneigh_done, hostname + data[1]) != -1) continue;

		push(babelneigh_done, hostname + data[1]);

		babelneigh_rtt({host: hostname, iface: data[1],}, data[7]);
		babelneigh_metric({host: hostname, iface: data[1],}, data[2]);
	}
}
fd.close();


// birdc show route count

let bird_routes_table_routes = gauge("bird_routes_table_routes");
let bird_routes_table_of_routes = gauge("bird_routes_table_of_routes");
let bird_routes_table_networks = gauge("bird_routes_table_networks");

fd = popen("birdc show route count");
while (true) {
	if (!fd) break;

	let line = fd.read('line');
	if (!line) break;

	if (index(line, "BIRD") == 0) continue;
	if (index(line, "Total") == 0) continue;

	let data = wsplit(line);
	bird_routes_table_routes( {table: data[9], }, data[0]);
	bird_routes_table_of_routes( {table: data[9], }, data[2]);
	bird_routes_table_networks( {table: data[9], }, data[5]);

}
fd.close();

// add true so that the collecter is considered a success.
true;
