import { readfile } from "fs";

let content = json(readfile("/tmp/pingbot.json") || "[]" );

let ping_packetloss = gauge("ping_packetloss");
let ping_avg = gauge("ping_avg");
let ping_min = gauge("ping_min");
let ping_max = gauge("ping_max");

for (let i = 0; i < length(content); i++) {
	ping_packetloss({server: content[i].server,}, content[i].packetloss);
	ping_avg({server: content[i].server,}, content[i].avg);
	ping_min({server: content[i].server,}, content[i].min);
	ping_max({server: content[i].server,}, content[i].max);
}
