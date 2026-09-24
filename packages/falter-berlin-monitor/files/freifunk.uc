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

// Location Pseudocode
// if uci.get('freifunk', 'monitor', 'show_on_map')  is true and uci.get('freifunk', 'location', 'latitude') is set  and uci.get('freifunk', 'location', 'longitude') is set {
//     gauge("freifunk_location")({
//         latitude: uci.get('freifunk', 'location', 'latitude') ,
//         longitude: uci.get('freifunk', 'location', 'longitude'),
//     }, 1);
// }

// olsr_links example: {"pid": 9736,"systemTime": 1771419359,"timeSinceStartup": 225082231,"configurationChecksum": "f40bfa31","links": [{"localIP": "10.36.197.152","remoteIP": "10.36.197.6","olsrInterface": "bbbdigger","ifName": "bbbdigger","validityTime": 137982,"symmetryTime": 123021,"asymmetryTime": 225205252,"vtime": 124000,"currentLinkStatus": "SYMMETRIC","previousLinkStatus": "SYMMETRIC","hysteresis": 0.000000,"pending": false,"lostLinkTime": 0,"helloTime": 0,"lastHelloTime": 0,"seqnoValid": false,"seqno": 0,"lossHelloInterval": 3000,"lossTime": 3521,"lossMultiplier": 65536,"linkCost": 4.250000,"linkQuality": 1.000000,"neighborLinkQuality": 0.234000}]}
const olsr_links = json(poneline("printf '/links' | nc 127.0.0.1 9090 2>/dev/null"));

for (let link in olsr_links.links) {
gauge("freifunk_olsr_link")({
        localIP: link.localIP,
        remoteIP: link.remoteIP,
        olsrInterface: link.olsrInterface,
        ifName: link.ifName,
        validityTime: link.validityTime,
        symmetryTime: link.symmetryTime,
        asymmetryTime: link.asymmetryTime,
        vtime: link.vtime,
        currentLinkStatus: link.currentLinkStatus,
        previousLinkStatus: link.previousLinkStatus,
        hysteresis: link.hysteresis,
        pending: link.pending,
        lostLinkTime: link.lostLinkTime,
        helloTime: link.helloTime,
        lastHelloTime: link.lastHelloTime,
        seqnoValid: link.seqnoValid,
        seqno: link.seqno,
        lossHelloInterval: link.lossHelloInterval,
        lossTime: link.lossTime,
        lossMultiplier: link.lossMultiplier,
        linkCost: link.linkCost,
        neighborLinkQuality: link.neighborLinkQuality
}, link.linkQuality);
}
