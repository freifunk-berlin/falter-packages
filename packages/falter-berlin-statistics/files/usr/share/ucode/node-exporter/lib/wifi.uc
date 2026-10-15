import { request, 'const' as wlconst } from 'nl80211';

const x = ubus.call("network.wireless", "status");

if (!x)
	return false;

const iftypes = [
	"Unknown",
	"Ad-Hoc",
	"Client",
	"Master",
	"Master (VLAN)",
	"WDS",
	"Monitor",
	"Mesh Point",
	"P2P Client",
	"P2P Go",
	"P2P Device",
	"OCB",
];

let m_radio_info = gauge("wifi_radio_info");
let m_network_info = gauge("wifi_network_info");
let m_network_quality = gauge("wifi_network_quality");
let m_network_bitrate = gauge("wifi_network_bitrate");
let m_network_noise = gauge("wifi_network_noise_dbm");
let m_network_signal = gauge("wifi_network_signal_dbm");
let m_stations_total = counter("wifi_stations_total");

for (let radio in x) {
	const rc = x[radio]["config"];

	m_radio_info({
		radio,
		htmode: rc["htmode"],
		channel: rc["channel"],
		country: rc["country"],
	} ,1);

	for (let iface in x[radio]["interfaces"]) {
		const ifname = iface["ifname"];
		const nc = iface["config"];
		const wif = request(wlconst.NL80211_CMD_GET_INTERFACE, 0, { dev: ifname });

		if (!wif)
			continue;

		m_network_info({
			radio,
			ifname,
			ssid: nc["ssid"] || nc["mesh_id"],
			bssid: wif["mac"],
			mode: iftypes[wif["iftype"]],
		}, 1);

		const wsta = request(wlconst.NL80211_CMD_GET_STATION, wlconst.NLM_F_DUMP, { dev: ifname });
		let signal = 0;
		let bitrate = 0;
		const stations = length(wsta) || 0;
		if (stations) {
			let rated = 0;
			for (let sta in wsta) {
				signal += sta["sta_info"].signal;
				const br = sta["sta_info"]["tx_bitrate"]?.bitrate32;
				if (br) {
					bitrate += br;
					rated++;
				}
			}
			bitrate = rated ? bitrate / (rated * 0.01) : 0;
			signal /= stations;
		}

		let labels = { radio, ifname };
		m_network_bitrate(labels, bitrate || NaN);
		m_network_signal(labels, signal || NaN);
		m_network_quality(labels, signal ? 100.0 / 70 * (signal + 110) : NaN);

		const wsur = request(wlconst.NL80211_CMD_GET_SURVEY, wlconst.NLM_F_DUMP, { dev: ifname });
		let noise = 0;
		for (let i in wsur) {
			if (i["survey_info"]["frequency"] != wif["wiphy_freq"])
				continue;

			noise = i["survey_info"]["noise"];
			break;
		}

		m_network_noise(labels, noise || NaN);

		if (config["stations"] != "1")
			continue;

		m_stations_total(labels, stations);
	}
}
