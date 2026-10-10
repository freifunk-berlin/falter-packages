import { popen } from 'fs';
let ubus = require("ubus");

let release = ubus.call("file", "read", { path: "/etc/freifunk_release", });
let m = match(release.data, /VARIANT=["'](.*?)["']/);
let VARIANT = m[1];

function exec(cmd) {
	let fh = popen(cmd, 'r');
	if (fh) { let r = trim(fh.read('all')); fh.close(); return r; }
	return '';
}

function ip_in_net(ip, prefix, cidr_mask) {
	let data = iptoarr(ip);
	if (!data) return false;
	let ip_int = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];

	data = iptoarr(prefix);
	if (!data) return false;
	let prefix_int = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];

	let mask = (int(cidr_mask) == 0) ? 0 :
		(0xFFFFFFFF << (32 - int(cidr_mask))) & 0xFFFFFFFF;
	return ((prefix_int & mask) == (ip_int & mask));
}

let local_ips = [];
let local_nets = [];
function is_local(ip) {
	if (length(local_ips) == 0) {
		// populate local_ips and local_nets
		let tables = [ 'main', 'local' ];
		for (let i = 0; i < length(tables) ; i++) {
			let fd = popen("ip route show table " + tables[i]);
			while (fd) {
				if (!fd) break;

				let line = fd.read('line');
				if (!line) break;

				let data = wsplit(line);
				let ip = data[0];
				if (ip == "default" || ip == "broadcast") continue;
				if (ip == "local")
					ip = data[1];
				if (index(ip, "/") == -1)
					push(local_ips, ip);
				else
					push(local_nets, ip);

			}
			fd.close();
		}
	}

	if ( index(local_ips, ip) != -1 ) {
		return true;
	}
	for (let i = 0; i < length(local_nets); i++) {
		let m = match(local_nets[i], /^([0-9.]+)\/([0-9]+)$/);
		if (!m) continue;
		if (ip_in_net(ip, m[1], m[2])) {
			return true;
		}
	}
	return false;
}

function get_bird_ip4_routes() {
	let result = [];
	let cmd = "birdc show route for 0.0.0.0/0";
	if (VARIANT == "gateway") cmd += " table v4_main";

	let fd = popen(cmd);
	while(true) {
		if (!fd) break;

		let line = fd.read('line');
		if (!line) break;

		// skip first two lines
		if (index(line, "BIRD") == 0) {
			fd.read('line'); // skip a line
			continue;
		}

		let data = wsplit(match(line, /(unicast.*)/)[1]);
		let line2 = fd.read('line');
		if (!line2) break; //should never happen
		let data2 = wsplit(trim(line2));

		let ip6_linklocal = trim(data[4], "]");
		let selected = data[5] == "*" ? 1 : 0;
		let metric = split(trim(data[5 + selected], "()"), "/")[1];
		let router_id = trim(data[6 + selected], "[]");
		let ip = data2[1];
		let iface = data2[3];

		// special case for static uplink
		if (trim(data[1], "[]") == "static_uplink") {
			ip6_linklocal = "STATIC_UPLINK";
			selected = data[3] == "*" ? 1 : 0;
			metric = trim(data[3 + selected], "()");
			router_id = "";
		}

		push(result, {
			ip6_linklocal: ip6_linklocal,
			selected: selected,
			metric: metric,
			router_id: router_id,
			ip: ip,
			iface: iface,
		});
	}
	fd.close();
	return result;
}


// get the ipv6 routes, including which one is selected

function get_bird_ip6_routes() {
	let result = [];
	let fd = popen("birdc show route for ::/0 from 2001:bf7::/32");
	while(true) {
		if (!fd) break;

		let line = fd.read('line');
		if (!line) break;

		// skip first two lines
		if (index(line, "BIRD") == 0) {
			fd.read('line');
			continue;
		}

		let data = wsplit(match(line, /(unicast.*)/)[1]);
		let line2 = fd.read('line');
		if (!line2) break; //should never happen
		let data2 = wsplit(trim(line2));

		let selected = (data[3] == "*") ? 1 : 0;
		let metric = split(trim(data[3 + selected], "()"), "/")[1];
		let router_id = trim(data[4 + selected], "[]");
		let ip6_linklocal = data2[1];
		let iface = data2[3];

		// special case for static uplink
		if (trim(data[1], "[]") == "static_v6_default_via_bgp") {
			ip6_linklocal = "STATIC_UPLINK";
			selected = data[3] == "*" ? 1 : 0;
			metric = trim(data[3 + selected], "()");
			router_id = "";
		}

		push(result, {
			ip6_linklocal: ip6_linklocal,
			selected: selected,
			metric: metric,
			router_id: router_id,
			iface: iface,
		});
	}
	fd.close();
	return result;
}


function get_babel_neighbors() {
	let result = [];
	let fd = popen("birdc show babel neighbors");
	while(true) {
		if (!fd) break;

		let line = fd.read('line');
		if (!line) break;

		if (index(line, "BIRD") == 0 ) {
			fd.read('line');
			fd.read('line');
			continue;
		}

		let data = wsplit(line);

		let ip6_linklocal = data[0];
		let iface = data[1];
		let metric = data[2];
		let rtt = data[7];

		push(result, {
			ip6_linklocal: ip6_linklocal,
			iface: iface,
			metric: metric,
			rtt: rtt,
		});
	}

	fd.close();
	return result;
}

function get_ip_neighbors(ver) {
	let result = [];
	let lines = exec("ip -j -" + ver +  " neigh show 2>/dev/null");
	if (!lines) return [];
	let neighbors = json(lines);
	if (!neighbors) return [];

	for (let i = 0; i < length(neighbors); i++) {
		if (neighbors[i].state[0] == "FAILED") continue;
		if (iptoarr(neighbors[i].dst) == null) continue;
		push(result, {
			addr: neighbors[i].dst,
			iface: neighbors[i].dev,
			mac: neighbors[i].lladdr,
		});
	}
	return result;
}

function get_gre_neighbors() {
	let result = [];
	let fd = popen('ip tunnel show');
	while (true) {
		if (!fd) break;

		let line = fd.read('line');
		if (!line) break;

		if (index(line, "gre4-") == 0) {
			let data = wsplit(line);
			if (iptoarr(data[3]) == null) continue;
			push(result, {
				iface: trim(data[0], ":"),
				ip: data[3],
			});
		}
	}
	fd.close();
	return result;
}

function get_wg_neighbors() {
	let result = [];
	let wg = poneline('wg show interfaces');
	if (wg) {
		let ifaces = wsplit(wg);
		for (let i = 0 ; i < length(ifaces) ; i++) {
			let fd = popen("ip r s t babel-ff dev " + ifaces[i]  + " | head -n 1");
			while (true) {
				if (!fd) break;

				let line = fd.read('line');
				if (!line) break;
				let data = wsplit(line);
				if (data[1] != "via") continue;
				if (iptoarr(data[2]) == null) continue;

				push(result, {
					iface: ifaces[i],
					ip: data[2],
				});
				break;
			}
		}
	}
	return result;
}

// collect bird/babel info

let bird_ip6_routes = get_bird_ip6_routes();
let bird_ip4_routes = get_bird_ip4_routes();
let babel_neighbors = get_babel_neighbors();

// get more info to be able to link ipv6 and ipv4 neighbors
let ip4_neighbors = get_ip_neighbors("4");
let ip6_neighbors = get_ip_neighbors("6");
let gre_neighbors = get_gre_neighbors();
let wg_neighbors = get_wg_neighbors();

let babel_ipv4_gateway_metric = gauge("babel_ipv4_gateway_metric");
let babel_ipv4_gateway_selection = gauge("babel_ipv4_gateway_selection");
let babel_ipv6_gateway_metric = gauge("babel_ipv6_gateway_metric");
let babel_ipv6_gateway_selection = gauge("babel_ipv6_gateway_selection");
let babelneigh_rtt = gauge("babel_neighbor_rtt");
let babelneigh_metric = gauge("babel_neighbor_metric");
let metric_done = [];

for (let i = 0; i < length(babel_neighbors) ; i++) {
	let linklocal = babel_neighbors[i].ip6_linklocal;
	let iface = babel_neighbors[i].iface;
	let metric = babel_neighbors[i].metric;
	let rtt = babel_neighbors[i].rtt;
	let remoteIP = "";
	let metric4 = 0;
	let selected4 = -1;
	let metric6 = 0;
	let selected6 = -1;

	// get IP addresses for wg interfaces
	if (match(iface, /^(wg_)|(ts_wg)/)) {
		// add special selected setting for wg tunneles on gw nodes
		if (match(iface, /^(wg_)/)) {
			selected4 = -2;
			selected6 = -2;
		}
		let f = filter(wg_neighbors, function(row) {
			return row.iface == iface;
		});
		if (length(f) > 0) remoteIP = f[0].ip;
	// get IP addresses for gre interfaces
	} else if (match(iface, /^(gre4-)/)) {
		selected4 = -3;
		selected6 = -3;
		let f = filter(gre_neighbors, function(row) {
			return row.iface == iface;
		});
		if (length(f) > 0) remoteIP = f[0].ip;
	}
	// get IP, selected and metrics for the routes
	let f = filter(bird_ip4_routes, function(row) {
		return row.ip6_linklocal == linklocal && row.iface == iface;
	});
	if (length(f) > 0) {
		remoteIP = f[0].ip;
		if (selected4 == -1) selected4 = f[0].selected;
		metric4 = f[0].metric;
	}
	f = filter(bird_ip6_routes, function(row) {
		return row.ip6_linklocal == linklocal && row.iface == iface;
	});
	if (length(f) > 0) {
		if (selected6 == -1 ) selected6 = f[0].selected;
		metric6 = f[0].metric;
	}

	// Get the ip address from the routing table
	if (remoteIP == "") {
		let f = filter(ip6_neighbors, function(row) {
			return row.addr == linklocal;
		});
		if (length(f) > 0) {
			let f2 = filter(ip4_neighbors, function(row) {
				return row.mac == f[0].mac;
			});
			if (length(f2) > 0) remoteIP = f2[0].addr;
		}
	}

	// we can't figure out an ip address, so give up
	if (remoteIP == "") continue;

	// make sure we don't repeat any metrics
	if (index(metric_done, iface + remoteIP) != -1) continue;
	push(metric_done, iface + remoteIP);

	// ignore local iP's, except on gw nodes (gre tunnels)
	if (VARIANT != "gateway") {
		if (is_local(remoteIP)) continue;
	}
	babel_ipv4_gateway_metric( {remoteIP: remoteIP, iface: iface,}, metric4);
	babel_ipv4_gateway_selection( {remoteIP: remoteIP, iface: iface,}, selected4);

	babel_ipv6_gateway_metric( {remoteIP: remoteIP, iface: iface,}, metric6);
	babel_ipv6_gateway_selection( {remoteIP: remoteIP, iface: iface,}, selected6);
	babelneigh_rtt({remoteIP: remoteIP, iface: iface,}, rtt);
	babelneigh_metric({remoteIP: remoteIP, iface: iface,}, metric);
}

// handle any static routes
let f = filter(bird_ip4_routes, function(row) {
	return row.ip6_linklocal == "STATIC_UPLINK";
});
if (length(f) > 0) {
	let remoteIP = f[0].ip;
	let selected4 = f[0].selected;
	let metric4 = f[0].metric;
	let iface = f[0].iface;
	let selected6 = -1;
	let metric6 = 0;
	let f2 = filter(bird_ip6_routes, function(row) {
		return row.ip6_linklocal == "STATIC_UPLINK" && row.iface == iface;
	});
	if (length(f2) > 0) {
		selected6 = f2[0].selected;
		metric6 = f2[0].metric;
	}
	babel_ipv4_gateway_metric( {remoteIP: remoteIP, iface: iface,}, metric4);
	babel_ipv4_gateway_selection( {remoteIP: remoteIP, iface: iface,}, selected4);

	babel_ipv6_gateway_metric( {remoteIP: remoteIP, iface: iface,}, metric6);
	babel_ipv6_gateway_selection( {remoteIP: remoteIP, iface: iface,}, selected6);
}


// birdc show route count

let bird_routes_table_routes = gauge("bird_routes_table_routes");
let bird_routes_table_of_routes = gauge("bird_routes_table_of_routes");
let bird_routes_table_networks = gauge("bird_routes_table_networks");

let fd = popen("birdc show route count");
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
