'use strict';

const uci = require("uci").cursor();
import { rand } from 'math';

const network = "network";
const mac_octets = 6;

function get(interface) {
	if (!interface) return false;
	return uci.get_all(network, interface);
}

function create(data) {
	if (!data) return false;
	if (!data.name) return false;
	if (!data.proto) return false;

	// check to see if the interface already exitsts.
	let section = uci.get(network, data.name);
	if (result) return false;

	result = uci.set(network, data.name, "interface");
	if (!result) return false;

	uci.set(network, data.name, "proto", data.proto);
	if (data.device) uci.set(network, data.name, "device", data.device);
	if (data.peerdns) uci.set(network, data.name, "peerdns", data.peerdns);
	if (data.ip4table) uci.set(network, data.name, "ip4table", data.ip4table);
	if (data.ip6table) uci.set(network, data.name, "ip6table", data.ip6table);
	if (data.ipaddr) {
		if (type(data.ipaddr) == "string")
			uci.set(network, data.name, "ipaddr", data.ipaddr);
		else if (type(data.ipaddr) == "array")
			for (let i = 0; i < length(data.ipaddr); i++) {
				uci.list_append(network, data.name,
					"ipaddr", data.ipaddr[i]);
			}
	}
	if (data.netmask) uci.set(network, data.name, "netmask", data.netmask);
	if (data.ip6addr) {
		if (type(data.ip6addr) == "string")
			uci.set(network, data.name, ip6addr, data.ip6addr);
		else if (type(data.ip6addr) == "array")
			for (let i = 0; i < length(data.ip6addr); i++) {
				uci.list_append(network, data.name,
					"ip6addr", data.ip6addr[i]);
			}
	}
	if (data.ip6assign) uci.set(network, data.name, "ip6assign", data.ip6assign);

}

function enable(interface) {
	if (!interface) return false;
	const result = uci.set(network, interface, "disabled", 0);
	if (result) uci.save(network);
	return result;
}

function disable(interface) {
	if (!interface) return false;
	const result = uci.set(network, interface, "disabled", 1);
	if (result) uci.save(network);
	return result;
}

function generate_hex_string(len, prefix) {                    
        // we assume any prefix is in the form XX:YY:ZZ      
        let result = prefix ? prefix + ":" : "";                           
        let start = (length(result)+1)/3;                                  
        for (let i = start; i < len ; i++) {                   
                result = result + sprintf("%02x", rand(hex("ff")+1)) + ":";    
        }                                           
        return substr(result, 0, -1); //drop the last ":"      
}

function generate_mac(prefix) {                              
        let result = generate_hex_string(mac_octets, prefix); 
        // ensure the MAC is unicast and locally administered  
        let first_octet = hex(substr(result, 0, 2));           
        first_octet = (first_octet | 0x02) & 0xFE; 
        return sprintf("%02x", first_octet) + substr(result, 2);
}                                                             

return {
	enable,
	disable,
	generate_hex_string,
	generate_mac
};
