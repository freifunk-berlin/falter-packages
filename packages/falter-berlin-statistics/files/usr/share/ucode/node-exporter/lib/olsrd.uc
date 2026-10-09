const olsrinfo = json(poneline("printf '/links/routes' | nc 127.0.0.1 9090 2>/dev/null"));

let linkCount = 0;
let lqSum = 0.0;
let nlqSum = 0.0;

let lqmetric = gauge("olsrd_link_signal_quality_lq");
let nlqmetric = gauge("olsrd_link_signal_quality_nlq");

for (let link in olsrinfo.links) {
	linkCount++;
	lqSum += link.linkQuality;
	nlqSum += link.neighborLinkQuality;
	lqmetric({localIP: link.localIP, remoteIP: link.remoteIP, ifName: link.ifName,}, link.linkQuality);
	nlqmetric({localIP: link.localIP, remoteIP: link.remoteIP, ifName: link.ifName,}, link.neighborLinkQuality);
}

gauge("olsrd_link_signal_quality_avg_lq")({metric: "average-lq",}, lqSum / linkCount);
gauge("olsrd_link_signal_quality_avg_nlq")({metric: "average-nlq",}, nlqSum / linkCount);

gauge("olsrd_links")(null, linkCount);

let routeCount = 0;
let etxSum = 0.0;
let metricSum = 0.0;

for (let route in olsrinfo.routes) {
	routeCount++;
	etxSum += route.etx;
	metricSum += route.metric;
}

gauge("olsrd_route_etx")(null, etxSum / routeCount);
gauge("olsrd_route_metric")(null, metricSum / routeCount);
gauge("olsrd_routes")(null, routeCount);

//let topologyCount = 0;
//let signalQualitySum = 0.0;

//for (let topology in olsrinfo.topology) {
//	topologyCount++;
//	signalQualitySum += topology.linkQuality;
//}
//
//gauge("olsrd_topology_signalquality")(null, signalQualitySum / topologyCount);
//gauge("olsrd_topology_links")(null, topologyCount);
