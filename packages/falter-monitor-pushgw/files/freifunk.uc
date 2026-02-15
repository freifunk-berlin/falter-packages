import { cursor } from "uci";

const uci = cursor();
uci.load("freifunk");

// contact
gauge("freifunk_contact")({
    name: uci.get('freifunk', 'contact', 'name'),
    nickname: uci.get('freifunk', 'contact', 'nickname'),
    mail: uci.get('freifunk', 'contact', 'mail'),
    phone: uci.get('freifunk', 'contact', 'phone'),
    homepage: uci.get('freifunk', 'contact', 'homepage'),
    note: uci.get('freifunk', 'contact', 'note'),
}, 1);

// Community

gauge("freifunk_community")({
    ssid: uci.get('freifunk', 'community', 'ssid'),
    mesh_network: uci.get('freifunk', 'community', 'mesh_network'),
    owm_api: uci.get('freifunk', 'community', 'owm_api'),
    name: uci.get('freifunk', 'community', 'name'),
    homepage: uci.get('freifunk', 'community', 'homepage'),
    longitude: uci.get('freifunk', 'community', 'longitude'),
    latitude: uci.get('freifunk', 'community', 'latitude'),
    ssid_schema: uci.get('freifunk', 'community', 'ssid_schema'),
    splash_network: uci.get('freifunk', 'community', 'splash_network'),
    splash_prefix: uci.get('freifunk', 'community', 'splash_prefix'),

}, 1);



// OLSR

const olsr_links = poneline("printf "/links" | nc 127.0.0.1 9090 2>/dev/null")


