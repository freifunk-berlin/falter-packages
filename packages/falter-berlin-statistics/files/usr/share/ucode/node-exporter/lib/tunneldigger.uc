'use strict';

import { popen, glob, basename } from "fs";
import { cursor } from "uci";

const uci = cursor();
const port = 8942;
const piddir = "/tmp/run/";
const td = "tunneldigger";

function get_td_established(port, pid) {
	let fd = popen("netstat -np | grep " + port);
	if (!fd) return [];

	let matches = [];
	while (true) {
		if (!fd) break;

		let line = fd.read('line');
		if (!line) break;

		let parts = wsplit(line);
		if (parts[6] == pid + "/" + td) {
			let src = split(parts[3], ":");
			let dst = split(parts[4], ":");
			push(matches, {src: src[0],
				sport: src[1],
				dst: dst[0],
				dport: dst[1],
			});
		}
	}
	fd.close();

	return matches;
}

function get_td_link(established) {
	let fd = fs.popen("grep -E \'" + port + ".*ASSURED\' /proc/net/nf_conntrack");
	let conns = [];
	while (true) {
		if (!fd) break;

		let line = fd.read('line');
		if (!line) break;

		let parts = wsplit(line);
		let src = split(parts[5], "=")[1];
		let dst = split(parts[6], "=")[1];
		let sport = split(parts[7], "=")[1];
		let dport = split(parts[8], "=")[1];
		let result = filter(established, function(row) {
			return ( (row.dst == dst) &&
				(row.src == src) &&
				(row.dport == dport) &&
				(row.sport == sport) )
		});

		if (result[0]) {
			push(conns, result[0]);
		}
	}

	fd.close();

	return conns;
}

let td_link = gauge("tunneldigger_link");

let pidglob = glob(piddir + td + "*.pid");
for (let pidfile in pidglob ){
	let regex = regexp("^" + td + "\.(.*)\.pid$");
	let m = match(basename(pidfile), regex);
	if (m) {
		let iface = m[1];
		let pid = poneline("cat " + pidfile);
		let established = get_td_established(port, pid);
		let link = get_td_link(established);

		if (!link) continue;
		if (length(link) > 1) continue;
		td_link({remoteIP: link[0].dst, ifName: iface}, 1);
	}
}
