#!/usr/bin/env ucode

let fs = require('fs');
let ubus = require('ubus').connect();
let uci = require('uci').cursor();
let resolv = require('resolv');
let uloop = require('uloop');

let cfg = {};

function load_config(name) {
	let o = uci.get_all(name, "owm");
	cfg = {
		"debug": o?.debug ? int(o.debug) != 0 : false,
	};
}

// severity: info, err, warn, debug. debug messages only shown when debug=1
function log(severity, msg) {
	if (severity === "debug" && !cfg.debug) {
		return;
	}
	system(sprintf("logger -t owm -p daemon.%s '%s'", severity, msg));
}

function exec(cmd) {
	let fh = fs.popen(cmd, 'r');
	if (fh) { let r = trim(fh.read('all')); fh.close(); return r; }
	return '';
}

function parse_olsr_links(json_str) {
	if (!json_str) return [];
	let data = json(json_str);
	if (!data || !data.links) return [];
	return map(filter(data.links, (l) => l.olsrInterface && !match(l.olsrInterface, /(wg|ts)_/)), (link) => ({
		sourceAddr4: link.localIP,
		destAddr4: link.remoteIP,
		id: link.remoteIP,
		quality: link.linkQuality
	}));
}

function get_babel_data() {
	let output = exec('birdc show babel neighbors');
	if (!output) return { neighbors: [], interfaces: {} };
	let lines = filter(map(split(output, '\n'), (x) => trim(x)), length);
	let neighbors = [];
	for (let i = 2; i < length(lines); i++) {
		let parts = filter(split(lines[i], ' '), length);
		if (length(parts) < 3) continue;
		if (!match(parts[0], /^fe80::/)) continue;
		push(neighbors, { ip: parts[0], iface: parts[1], metric: int(parts[2]) });
	}
	let interfaces = {};
	let iface_out = exec('birdc show babel interfaces');
	if (iface_out) {
		let iface_lines = filter(map(split(iface_out, '\n'), (x) => trim(x)), length);
		for (let j = 2; j < length(iface_lines); j++) {
			let parts = filter(split(iface_lines[j], ' '), length);
			if (length(parts) < 8) continue;
			if (!parts[6] || !iptoarr(parts[6])) continue;
			interfaces[parts[0]] = { local_ipv4: parts[6] };
		}
	}
	return { neighbors, interfaces };
}

function build_neigh_tables() {
	let ipv4 = {}, ipv6 = {};
	let parse_json = (cmd, map_ipv6) => {
		let out = exec(cmd);
		if (!out) return;
		let entries = json(out);
		if (!entries) return;
		for (let i = 0; i < length(entries); i++) {
			let e = entries[i];
			if (!e.lladdr) continue;
			if (map_ipv6) ipv6[e.dst] = e.lladdr;
			else ipv4[e.lladdr] = e.dst;
		}
	};
	parse_json('ip -j -4 neigh show 2>/dev/null', false);
	parse_json('ip -j -6 neigh show 2>/dev/null', true);
	return { ipv4, ipv6 };
}

function parse_babel_links(neighbors, interfaces, ipv4_neigh, ipv6_neigh) {
	let links = [];
	for (let n in neighbors) {
		if (match(n.iface, /(wg|ts)_/)) continue;
		let local_ipv4 = interfaces[n.iface]?.local_ipv4;
		if (!local_ipv4) continue;
		let mac = ipv6_neigh[n.ip];
		if (!mac) continue;
		let neighbor_ipv4 = ipv4_neigh[mac];
		if (!neighbor_ipv4) continue;
		push(links, {
			sourceAddr4: local_ipv4,
			destAddr4: neighbor_ipv4,
			id: neighbor_ipv4,
			quality: (n.metric > 0 && n.metric < 65534) ? 256 / n.metric : 0.01
		});
	}
	return links;
}

function resolve_hostname(ip) {
	let result = resolv.query(ip, { type: ['PTR'] });
	for (let domain in result) {
		let ptr = result[domain]?.PTR?.[0];
		if (!ptr) continue;
		let m = match(ptr, /([^.]+)\.ff$/);
		if (m) return m[1] + '.olsr';
	}
	return ip;
}

function send_to_server(json_str, hostname) {
	let server = 'api.openwifimap.net';
	
	let try_ip = (ip) => {
		log('debug', 'trying OWM server ' + ip);
		let resp = exec('uclient-fetch -q --method=PUT --header="Content-Type: application/json" --body-data=\'' + json_str + '\' -O - "http://' + ip + '/update_node/' + hostname + '.olsr" 2>&1');
		if (index(resp, '200') >= 0 || index(resp, 'OK') >= 0) { log('info', 'OWM update successful'); return true; }
		log('debug', 'OWM upload to ' + ip + ' failed');
		return false;
	};
	
	for (let type in [['AAAA'], ['A']]) {
		let result = resolv.query(server, { type });
		for (let d in result) {
			let ips = type[0] === 'AAAA' ? (result[d]?.AAAA || []) : (result[d]?.A || []);
			for (let ip in ips) if (try_ip(ip)) return true;
		}
	}
	
	log('err', 'OWM update failed: could not connect to server');
	return false;
}

function concat(a, b) {
	let r = [];
	for (let i = 0; i < length(a); i++) push(r, a[i]);
	for (let i = 0; i < length(b); i++) push(r, b[i]);
	return r;
}

function read_freifunk_release() {
	let fh = fs.popen('cat /etc/freifunk_release 2>/dev/null', 'r');
	if (!fh) return {};
	let content = trim(fh.read('all'));
	fh.close();
	
	let result = {};
	let distrib_id = match(content, /FREIFUNK_DISTRIB_ID="([^"]*)"/);
	let release = match(content, /FREIFUNK_RELEASE='([^']*)'/);
	let revision = match(content, /FREIFUNK_REVISION='([^']*)'/);
	
	if (distrib_id) result.distrib_id = distrib_id[1];
	if (release) result.release = release[1];
	if (revision) result.revision = revision[1];
	
	return result;
}

function format_kernel_date(timestamp) {
	if (!timestamp) return '';
	let cmd = sprintf('date -u -d @%d', timestamp);
	let fh = fs.popen(cmd, 'r');
	if (!fh) return '';
	let result = trim(fh.read('all'));
	fh.close();
	result = replace(result, 'UTC ', '');
	result = 'SMP ' + result;
	return result;
}

function build_json_data() {
	let board = ubus.call('system', 'board');
	let info = ubus.call('system', 'info');
	let hostname = board.hostname;
	let latitude = uci.get('system', '@system[0]', 'latitude');
	let longitude = uci.get('system', '@system[0]', 'longitude');
	
	if (!latitude || !longitude) {
		log('warn', 'OWM update disabled: latitude/longitude not configured');
		return null;
	}
	
	let olsr_links = parse_olsr_links(exec('uclient-fetch -q -O - http://127.0.0.1:9090/links'));
	let local_ips = {};
	let babel_links = [];
	let bird_status = exec('birdc show status');
	
	if (bird_status && index(bird_status, 'Router ID') >= 0) {
		let babel = get_babel_data();
		for (let k in babel.interfaces) {
			if (babel.interfaces[k].local_ipv4) local_ips[babel.interfaces[k].local_ipv4] = true;
		}
		let tables = build_neigh_tables();
		babel_links = parse_babel_links(babel.neighbors, babel.interfaces, tables.ipv4, tables.ipv6);
	}
	
	let combined = concat(babel_links, olsr_links);
	let filtered = [];
	for (let i = 0; i < length(combined); i++) {
		let l = combined[i];
		if (l && l.destAddr4 && !(l.destAddr4 in local_ips) && l.sourceAddr4 !== l.destAddr4) push(filtered, l);
	}
	let best = {};
	for (let i = 0; i < length(filtered); i++) {
		let link = filtered[i];
		let key = resolve_hostname(link.id);
		link.id = key;
		if (!best[key] || link.quality > best[key].quality) best[key] = link;
	}
	let all_links = map(keys(best), (k) => best[k]);
	
	let ff_release = read_freifunk_release();
	let firmware_name = ff_release.distrib_id ? 
		(ff_release.distrib_id + ' ' + ff_release.release) : 
		(board.release.distribution + ' ' + board.release.version);
	let firmware_rev = ff_release.revision || board.release.revision;
	
	let json_data = {
		freifunk: { contact: {}, community: {} },
		type: 'node', script: 'owm', api_rev: 1,
		system: { sysinfo: ['system is deprecated', board.model], uptime: [info.uptime], loadavg: [info.load[1] * 1.0 / 65536] },
		olsr: { ipv4Config: {} },
		links: all_links,
		latitude: latitude * 1, longitude: longitude * 1,
		hostname, updateInterval: 1800, hardware: board.system,
		firmware: { name: firmware_name, revision: firmware_rev, kernelVersion: board.kernel, kernelBuildDate: format_kernel_date(board.release.builddate) }
	};
	
	let contact_keys = ['name', 'nickname', 'mail', 'phone', 'homepage', 'note'];
	let community_keys = ['name', 'homepage', 'ssid', 'mesh_network', 'owm_api', 'longitude', 'latitude', 'ssid_scheme', 'splash_network', 'splash_prefix'];
	for (let i = 0; i < length(contact_keys); i++) {
		let v = uci.get('freifunk', 'contact', contact_keys[i]);
		if (v) json_data.freifunk.contact[contact_keys[i]] = v;
	}
	for (let i = 0; i < length(community_keys); i++) {
		let v = uci.get('freifunk', 'community', community_keys[i]);
		if (v) json_data.freifunk.community[community_keys[i]] = v;
	}
	
	let olsr_cfg = exec('uclient-fetch -q -O - http://127.0.0.1:9090/config');
	if (olsr_cfg) {
		let cfg_json = json(olsr_cfg);
		if (cfg_json) json_data.olsr.ipv4Config = cfg_json;
	}
	
	return json_data;
}

function run_owm() {
	let json_data = build_json_data();
	if (!json_data) return false;
	let json_str = sprintf('%J', json_data);
	if (cfg.debug) {
		let fh = fs.open("/tmp/owm-debug.json", "w");
		fh.write(json_str);
		fh.close();
		log('debug', "/tmp/owm-debug.json written");
	}
	return send_to_server(json_str, json_data.hostname);
}

load_config("owm");

// only shown when debug=1 (handled in log function)
log("debug", "debug logging enabled");

uloop.init();

// run first upload after 5min, next at 30min, then every 30min
uloop.timer(300000, () => {
	run_owm();
	log("info", "initial OWM update done, next in 25min, then every 30min");
});

// regular 30min interval uploads
uloop.interval(1800000, () => {
	run_owm();
});

uloop.run();
