#!/usr/bin/ucode

const fs = require("fs");
const uci = require("uci");

// from luci-app-ffwizard-falter/root/etc/uci-defaults/wizarddefaults
// and luci-app-policyrouting/luasrc/model/cbi/freifunk/policyrouting.lua
function initPolicyRouting(ctx) {
  ctx.set("freifunk-policyrouting", "pr", "enable", "1");
  ctx.set("freifunk-policyrouting", "pr", "strict", "1");
  ctx.set("freifunk-policyrouting", "pr", "fallback", "1");
  ctx.set("freifunk-policyrouting", "pr", "zones", ["freifunk"]);
}

// from luci-app-ffwizard-falter/luasrc/tools/freifunk/assistant/ffwizard.lua
function configureQOS(ctx, cfg) {
  if cfg.share {
    ctx.delete("qos", "wan");
    ctx.delete("qos", "lan");
    ctx.delete("qos", "ffuplink");
    ctx.set("qos", "ffuplink", "enabled", "1");
    ctx.set("qos", "ffuplink", "classgroup", "Default");
    ctx.set("qos", "ffuplink", "upload", cfg.upload);
    ctx.set("qos", "ffuplink", "download", cfg.download);
    let sgwspeed = sprintf("%d %d", cfg.upload, cfg.download);
    ctx.set("olsrd", "olsrd", "SmartGatewaySpeed", sgwspeed);
    ctx.set("olsrd", "olsrd", "SmartGatewayUplink", "both");
  }
}

// from luci-app-ffwizard-falter/luasrc/model/cbi/freifunk/assistant/generalInfo.lua
function generalInfo(ctx, cfg) {
  ctx.set("freifunk", "contact", "nickname", cfg.contact.nickname);
  ctx.set("freifunk", "contact", "name", cfg.contact.realname);
  ctx.set("freifunk", "contact", "mail", cfg.contact.email);
  ctx.set("freifunk", "contact", "location", "");

  ctx.set("system", "@system[0]", "cronloglevel", "10");
  ctx.set("system", "@system[0]", "zonename", "Europe/Berlin");
  ctx.set("system", "@system[0]", "timezone", "CET-1CEST,M3.5.0,M10.5.0/3");
  ctx.set("system", "@system[0]", "hostname", cfg.node.name);

  // TODO: merge community profile
  // TODO: set lat+lon+alt
}

// from luci-app-ffwizard-falter/luasrc/model/cbi/freifunk/assistant/shareInternet.lua
function shareInternet(ctx, cfg) {
  // XXX: what about sharenet=2
  ctx.set("ffwizard", "settings", "sharenet", cfg.share ? 1 : 0);
  ctx.set("ffwizard", "settings", "usersBandwidthUp", cfg.upload);
  ctx.set("ffwizard", "settings", "usersBandwidthDown", cfg.download);
}

// from luci-app-ffwizard-falter/luasrc/model/cbi/freifunk/assistant/optionalConfigs.lua
function optionalConfigs(ctx, cfg) {
  ctx.set("ffwizard", "settings", "enableStats", cfg.node.monitoring ? 1 : 0);
}

// from luci-app-ffwizard-falter/luasrc/model/cbi/freifunk/assistant/wireless.lua
function wireless(ctx, cfg) {

}

// from luci-app-ffwizard-falter/luasrc/controller/assistant/assistant.lua
function commit() {
  configureQOS(ctx, cfg.node.internet);
  initPolicyRouting(ctx);
}

//

let f = fs.open("ffwizard3.json");
let cfg = json(f);
f.close();

let ctx = uci.cursor();

generalInfo(ctx, cfg);
shareInternet(ctx, cfg.node.internet);
