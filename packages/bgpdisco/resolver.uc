'use strict';

import * as fs from 'fs';
import { rand } from 'math';
const struct = require("struct");

function open_tempfile() {
	const prefix = "/tmp/lookup";

	let i=0;
        // attempt 10 times
	while (i<10) {
	        let suffix = sprintf("%04x", rand(hex("ffff")));
		let filename = prefix + suffix;

		let fh = fs.open(filename, 'wx');
		if (fh) {
			return { fh: fh, filename: filename };
		}
		i++;
	}

	return null;
}

function close_tempfile(data) {
	if (!data) return;

	data.fh.close();
	fs.unlink(data.filename);
}

function strtohex(input) {
	let result = "";
	for (let i=0; i<length(input); i++) {
		result = result + hexenc(substr(input,i,1)) + ":";
	}
	return substr(result, 0, -1); // drop the last ":"
}

function bytes_to_ipv6(bytes, start_idx) {
	let result = "";
	for (let i = 0; i < 8; i++) {
		let word_arr = struct.unpack(">H", substr(bytes, start_idx + (i * 2), 2));
		result = result + sprintf("%04x", word_arr[0]) + ":";
	}
	return substr(result, 0, -1);
}

// key should be (for example with ak36-gw) in the format of either
// for an IP address
//   'net = 10.31.130.160/32'
//   'net = 2001:bf7:750:4000::/128'
// for a hostname (example ak36-gw)
//   '5B:20:5B:20:30:2C:20:22:61:6B:33:36:2D:67:77:22:20:5D:20:5D = bgp_unknown_0xfa'
function mrt_dump(filename, key) {
	let cmd = "birdc mrt dump table '\"*bgpdisco\"' to '\"" +
		filename + "\"' where " + key;
	let process = fs.popen(cmd, 'r');
	if (process) {
		process.read('all');
		process.close();
	}
	else
	{
		print("birdc execution error: ", process.error(), "\n");
	}
	return;
}

function mrt_parse(filename) {
	const MRT_HEADER_SIZE = 12;
	const MRT_TYPE_TABLE_DUMP_V2 = 13;
	const MRT_SUBTYPE_RIB_IPV4_UNICAST = 2;
	const MRT_SUBTYPE_RIB_IPV6_UNICAST = 4;
	const MRT_ENTRY_COUNT = 2;

	const RIB_SEQ_NR = 4;
	const RIB_PREFIX_LEN = 1;
	const RIP_ENTRY_COUNT = 2;

	let result = [];
	let fh = fs.open(filename, 'r');

	while (true) {
		let header_raw = fh.read(MRT_HEADER_SIZE);
		if (!header_raw || length(header_raw) < MRT_HEADER_SIZE) break;

		let header = struct.unpack(">I H H I", header_raw);
		let type = header[1];
		let subtype = header[2];
		let len = header[3];

		let payload = fh.read(len);
		if (!payload || length(payload) > len) break;

		if (type == MRT_TYPE_TABLE_DUMP_V2 &&
		  (subtype == MRT_SUBTYPE_RIB_IPV4_UNICAST ||
		   subtype == MRT_SUBTYPE_RIB_IPV6_UNICAST) ) {
			let idx = RIB_SEQ_NR;
			let prefix_len_arr = struct.unpack(">B", substr(payload, idx, RIB_PREFIX_LEN));
			let prefix_len = prefix_len_arr[0];
			let prefix_bytes_len = int((prefix_len + 7) / 8);
			idx += RIB_PREFIX_LEN;

			let prefix_ip = "";

			if (subtype == MRT_SUBTYPE_RIB_IPV4_UNICAST) {
				// IPv4
				let p_bytes = [];
				for (let b = 0; b < prefix_bytes_len; b++) {
					let b_arr = struct.unpack(">B", substr(payload, idx + b, 1));
					p_bytes[b] = b_arr[0];
				}
				for (let b = prefix_bytes_len; b < 4; b++) {
					p_bytes[b] = 0;
				}
				prefix_ip = sprintf("%d.%d.%d.%d/%d", p_bytes[0], p_bytes[1], p_bytes[2], p_bytes[3], prefix_len);
				} else {
				// IPv6
				let v6_buf = substr(payload, idx, prefix_bytes_len);
				// Pad the buffer
				while (length(v6_buf) < 16) v6_buf += "\x00";
				prefix_ip = sprintf("%s/%d", bytes_to_ipv6(v6_buf, 0), prefix_len);
			}

			idx += prefix_bytes_len;
			let entry_count_arr = struct.unpack(">H", substr(payload, idx, MRT_ENTRY_COUNT));
			let entry_count = entry_count_arr[0];
			idx += MRT_ENTRY_COUNT;

			let hostname = "";
			for (let e = 0; e < entry_count ; e++) {
				const RIB_PEER_INDEX = 2;
				const RIB_OTIME = 4;
				const RIB_ATTR_LEN = 2;
				const BGP_TABLE_250 = 250; // bgp_unknown_0xfa

				let attr_len_arr = struct.unpack(">H", substr(payload, idx + RIB_PEER_INDEX + RIB_OTIME, RIB_ATTR_LEN));
				let attr_len = attr_len_arr[0];
				idx += RIB_PEER_INDEX + RIB_OTIME + RIB_ATTR_LEN; // Move past path metadata headers

				let attr_end = idx + attr_len;

				while (idx < attr_end) {
					let flags_arr = struct.unpack(">B", substr(payload, idx, 1));
					let flags = flags_arr[0];
					let attr_type_arr = struct.unpack(">B", substr(payload, idx + 1, 1));
					let attr_type = attr_type_arr[0];
					idx += 2;

					let ext_len = (flags & 0x10);
					let current_attr_len = 0;
					if (ext_len) {
						let cur_len_arr  = struct.unpack(">H", substr(payload, idx, 2));
						current_attr_len = cur_len_arr[0];
						idx += 2;
					} else {
						let cur_len_arr  = struct.unpack(">B", substr(payload, idx, 1));
						current_attr_len = cur_len_arr[0];
						idx += 1;
					}

					let next_idx = idx + current_attr_len;

					if (attr_type == BGP_TABLE_250) {  //here we have the hostnames
						let raw_chunk = trim(substr(payload, idx, current_attr_len));
						let data = json(raw_chunk);
						if (data) {
							hostname = data[0][-1];
						} else {
							hostname = raw_chunk;
						}
					}
					idx = next_idx;
				}
			}
			push(result, {
					ip_version: (subtype == MRT_SUBTYPE_RIB_IPV4_UNICAST) ? 4 : 6,
					ip_address: prefix_ip,
					hostname: hostname
			});
		}
	}

	return result;
}

function lookup(query) {

	let tempfile = open_tempfile();
	mrt_dump(tempfile.filename, query);
	let result = mrt_parse(tempfile.filename);
	close_tempfile(tempfile);
	return result;
}

function resolve(str) {
	let ip = iptoarr(str);
	let query = "";
	if (ip) {
		let mask = "/32";
		if (length(ip) == 16) mask = "/128";
		query = "'net = " + str + mask + "'";
	} else {
		query = "[ [ 0, \"" + str + "\" ] ]";
		query = "'" + strtohex(query) + " = bgp_unknown_0xfa\'";
	}
	let result = lookup(query);
	return result;
}

return {
	resolve: resolve
};

