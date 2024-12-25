#!/usr/bin/ucode

// TODO:
// - [x] procd service
// - [x] config via uci
// - [x] load keys from file
// - [x] bug: high cpu usage
// - [x] generate private keys
// - [x] don't abort for common failures
// - [x] test olsrd and babel
// - [x] test multiple ifaces
// - [x] bug: possible multiple use of servers
// - [x] better logging
// - [x] handle dhcp renewals
// - [ ] disable strom temporarily
// - [ ] nftables rules for mss clamping
// - [ ] retry dhcp on boot
// - [ ] less logging
// - [x] implement insecure_cert option
// - [ ] implement disabled option
// - [ ] warn if ipv6 RA is disabled

const uloop = require("uloop");
const rtnl = require("rtnl");
const wg = require("wireguard");
const fs = require("fs");
const math = require("math");
const uci = require("uci");

const UPLINK_NETNS_IFNAME = 'ts_uplink';
const WG_LOGIN = { "username": "wginstaller", "password": "wginstaller" };

let cfg = {};

function load_config(name) {
  let ctx = uci.cursor();

  let ts = ctx.get_all(name, "tunspace");
  let cfg = {
    "debug": int(ts.debug) != 0,
    "uplink_netns": ""+ts.uplink_netns,
    "uplink_ifname": ""+ts.uplink_ifname,
    "uplink_mode": ""+ts.uplink_mode,
    "maintenance_interval": int(ts.maintenance_interval),
    "wireguard_servers": {},
    "wireguard_interfaces": {},
    "l2tp_servers": {},
    "l2tp_interfaces": {},
    "plain_interfaces": {},
  };

  ctx.foreach(name, "wg-server", function(c) {
    cfg.wireguard_servers[""+c.name] = {
      "url": ""+c.url,
      "insecure_cert": int(c.insecure_cert) != 0,
      "disabled": int(c.disabled) != 0,
    };
  });

  ctx.foreach(name, "wg-interface", function(c) {
    cfg.wireguard_interfaces[""+c.ifname] = {
      "ipv6": ""+c.ipv6,
      "ipv4": ""+c.ipv4,
      "mtu": int(c.mtu),
      "port": int(c.port),
      "disabled": int(c.disabled) != 0,
    };
  });

  return cfg;
}

function log(msg) {
  printf(msg+"\n");
  system(sprintf("logger -t tunspace '%s'", msg));
}

function debug(msg) {
  if (cfg.debug) {
    log(msg);
  }
}

function rtnl_request(cmd, flags, msg) {
  let reply = rtnl.request(cmd, flags, msg);
  debug(sprintf("rtnl: cmd=%J flags=%J msg=%J error=%J reply=%s", cmd, flags, msg, err, type(reply)));
  return reply;
}

function wg_request(cmd, flags, msg) {
  let reply = wg.request(cmd, flags, msg);
  if (length(msg.privateKey) > 0) {
    msg.privateKey = "REDACTED";
  }
  debug(sprintf("wireguard: cmd=%J flags=%J msg=%J error=%J reply=%s", cmd, flags, msg, err, type(reply)));
  return reply;
}

function shell_command(cmd) {
  let exit = system(cmd);
  debug(sprintf("%s (exit=%d)", cmd, exit));
  return exit;
}

function create_namespace(st, nsname) {
  let p = fs.popen("ip -j netns list-id", "r");
  let out = p.read("all");
  p.close();
  debug("ip -j netns list-id (error="+p.error()+")");
  if (out == null) {
    return false;
  }
  let ids = json(out);
  for (ns in ids) {
    if (ns.name == nsname) {
      st.nsid = ns.nsid;
      return true;
    }
  }
  st.nsid = math.rand();
  return 0 == shell_command("ip netns add "+nsname+" && ip netns set "+nsname+" "+st.nsid);
}

function interface_exists(ifname) {
  let reply = rtnl_request(rtnl.const.RTM_GETLINK, rtnl.const.NLM_F_REQUEST, {
    "ifname": ifname,
  });
  rtnl.error(); // throw the error away
  return !!reply;
}

function interface_exists_netns(ifname, netns) {
  // TODO: ucode-mod-rtnl doesn't support target_netnsid yet
  //
  // let reply = rtnl_request(rtnl.const.RTM_GETLINK, rtnl.const.NLM_F_REQUEST, {
  //   "ifname": ifname,
  //   "target_netnsid": nsid,
  // });
  // return !!reply;
  return 0 == shell_command("ip -n "+netns+" link show "+ifname+" >/dev/null 2>/dev/null");
}

function create_wg_interface(nsid, ifname, ifcfg, netns) {
  if (interface_exists(ifname)) {
    return true;
  }

  if (!interface_exists_netns(ifname, netns)) {
    // TODO: use once ucode-mod-rtnl supports target_netnsid
    //
    // rtnl_request(rtnl.const.RTM_NEWLINK,
    //              rtnl.const.NLM_F_REQUEST|rtnl.const.NLM_F_CREATE|rtnl.const.NLM_F_EXCL, {
    //   "target_netnsid": nsid,
    //   "ifname": ifname,
    //   "linkinfo": {
    //     "type": "wireguard",
    //   },
    //   "mtu": ifcfg.mtu,
    //   // TODO: probably only supported through ioctl...
    //   //
    //   // "flags": rtnl.const.IFF_UP|rtnl.const.IFF_POINTOPOINT|rtnl.const.IFF_NOARP,
    //   // "change": rtnl.const.IFF_UP,
    // });
    // if (rtnl.error()) {
    //   return false;
    // }
    if (0 != shell_command("ip -n "+netns+" link add "+ifname+" type wireguard")) {
      return false;
    }
  }

  // TODO: not supported in kernel yet...
  //       see https://lore.kernel.org/all/20191107132755.8517-7-jonas@norrbonn.se/T/
  //
  // let reply = rtnl_request(rtnl.const.RTM_SETLINK,
  //                          rtnl.const.NLM_F_REQUEST|rtnl.const.NLM_F_EXCL, {
  //   "target_netnsid": nsid,
  //   "ifname": ifname,
  //   "net_ns_pid": 1,
  // });
  // if (rtnl.error()) {
  //   return false;
  // }
  if (0 != shell_command("ip -n "+netns+" link set "+ifname+" netns 1")) {
    return false;
  }

  // set mtu and bring the interface up
  if (0 != shell_command("ip link set "+ifname+" mtu "+ifcfg.mtu)) {
    return false;
  }
  if (0 != shell_command("ip link set up "+ifname)) {
    return false;
  }

  // configure wireguard
  wg_request(wg.const.WG_CMD_SET_DEVICE, wg.const.NLM_F_REQUEST, {
    "ifname": ifname,
    "listenPort": ifcfg.port,
  });
  if (err = wg.error()) {
    log("WG_CMD_SET_DEVICE failed: "+err);
    return false;
  }

  // add ipv6 address
  if (length(ifcfg.ipv6) > 0) {
    rtnl_request(rtnl.const.RTM_NEWADDR,
                 rtnl.const.NLM_F_REQUEST|rtnl.const.NLM_F_CREATE|rtnl.const.NLM_F_EXCL, {
      "dev": ifname,
      "family": rtnl.const.AF_INET6,
      "address": ifcfg.ipv6,
    });
    if (err = rtnl.error()) {
      log("RTM_NEWADDR with AF_INET6 failed: "+err);
      return false;
    }
  }

  // add ipv4 address
  if (length(ifcfg.ipv4) > 0) {
    rtnl_request(rtnl.const.RTM_NEWADDR,
                 rtnl.const.NLM_F_REQUEST|rtnl.const.NLM_F_CREATE|rtnl.const.NLM_F_EXCL, {
      "dev": ifname,
      "label": ifname,
      "family": rtnl.const.AF_INET,
      "address": ifcfg.ipv4,
      "local": split(ifcfg.ipv4, "/")[0],
    });
    if (err = rtnl.error()) {
      log("RTM_NEWADDR with AF_INET failed: "+err);
      return false;
    }
  }

  return true;
}

function wg_interface_ok(st, ifname) {
  return st.interfaces[ifname]
    && 0 == shell_command("ping -c 3 -w 3 -A fe80::1%"+ifname+" >/dev/null");
}

function wg_replace_endpoint(ifname, cfg, next) {
  let ifcfg = cfg.wireguard_interfaces[ifname];
  let srvcfg = cfg.wireguard_servers[next];
  let certopt = srvcfg.insecure_cert ? "--no-check-certificate" : "";

  // generate a fresh private key
  let randfd = fs.open("/dev/random");
  let privkey = randfd.read(32);
  randfd.close();
  if (length(privkey) < 32) {
    log("failed to read 32 bytes from /dev/random");
    return false;
  }
  let reply = wg_request(wg.const.WG_CMD_SET_DEVICE, wg.const.NLM_F_REQUEST, {
    "ifname": ifname,
    "privateKey": b64enc(privkey),
  });
  if (err = wg.error()) {
    log("WG_CMD_SET_DEVICE failed: "+err);
    return false;
  }

  // get the public key for registration
  let reply = wg_request(wg.const.WG_CMD_GET_DEVICE,
                         rtnl.const.NLM_F_REQUEST|rtnl.const.NLM_F_DUMP, {
    "ifname": ifname,
  });
  if (err = wg.error()) {
    log("WG_CMD_GET_DEVICE failed: "+err);
    return false;
  }
  if (length(reply) < 1) {
    log("can't replace wireguard endpoint, interface "+ifname+" not found");
    return false;
  }
  let pubkey = reply[0].publicKey;

  // ubus login on the tunnel server
  let msg = {
    "jsonrpc": "2.0",
    "id": 1,
    "method": "call",
    "params": [
      "00000000000000000000000000000000",
      "session",
      "login",
      WG_LOGIN]};
  let cmd = sprintf("ip netns exec %s uclient-fetch -q -O - %s --post-data='%s' %s", cfg.uplink_netns, certopt, "%s", srvcfg.url);
  let p = fs.popen(sprintf(cmd, msg), "r");
  let out = p.read("all");
  if (substr(out, 0, 1) != "{") {
    log(sprintf(cmd+" (error=unexpected data, data=%s)", "...", out));
    return false;
  } else {
    debug(sprintf(cmd+" (error=%s)", "...", p.error()));
  }
  let reply = json(out);
  if (reply.result[0] != 0) {
    log(sprintf(cmd+" (error=unexpected content, data=%s)", "...", out));
    return false;
  }
  let sid = reply.result[1].ubus_rpc_session;

  // tunnel registration
  let msg = {
    "jsonrpc": "2.0",
    "id": 1,
    "method": "call",
    "params": [
      sid,
      "wginstaller",
      "register",
      { "public_key": pubkey, "mtu": ifcfg.mtu },
    ],
  };
  let cmd = sprintf("ip netns exec %s uclient-fetch -q -O - %s --post-data='%s' %s", cfg.uplink_netns, certopt, "%s", srvcfg.url);
  let p = fs.popen(sprintf(cmd, msg), "r");
  let out = p.read("all");
  if (substr(out, 0, 1) != "{") {
    log(sprintf(cmd+" (error=unexpected data, data=%s)", "...", out));
    return false;
  } else {
    debug(sprintf(cmd+" (error=%s)", "...", p.error()));
  }
  let reply = json(out);
  if (!reply.result || reply.result[0] != 0 || reply.result[1].response_code != 0) {
    // response_code 1 means "public key is already used"
    // see wg-installer/wg-server/lib/wg_functions.sh
    log(sprintf(cmd+" (error=unexpected content, data=%s)", "...", out));
    return false;
  }

  let peer = {
    "public_key": reply.result[1].gw_pubkey,
    "endpoint": replace(srvcfg.url, regexp('^https?://([^/]+).*$'), '$1:'+reply.result[1].gw_port),
  };

  // set tunnel server as our peer
  let reply = wg_request(wg.const.WG_CMD_SET_DEVICE, wg.const.NLM_F_REQUEST, {
    "ifname": ifname,
    "flags": wg.const.WGDEVICE_F_REPLACE_PEERS,
    "peers": [{
      "endpoint": peer.endpoint,
      "publicKey": peer.public_key,
      "persistentKeepaliveInterval": 15,
      "allowedips": [{
        "family": rtnl.const.AF_INET6,
        "ipaddr": "::",
        "cidrMask": 0,
      },{
        "family": rtnl.const.AF_INET,
        "ipaddr": "0.0.0.0",
        "cidrMask": 0,
      }],
    }],
  });
  if (err = wg.error()) {
    log("WG_CMD_SET_DEVICE failed: "+err);
    return false;
  }
  return true;
}

function wireguard_maintenance(st, cfg) {
  for (ifname, ifcfg in cfg.wireguard_interfaces) {
    let in_use = map(values(st.interfaces), (ifst) => ifst.server);
    let current = (st.interfaces[ifname] && st.interfaces[ifname].server) || null;
    if (wg_interface_ok(st, ifname)) {
      debug(sprintf("tunnel %s -> %s is healthy", ifname, current));
      continue;
    }

    // refill candidates if neccessary, but skip servers that are already in use
    if (length(st.candidates) == 0) {
      st.candidates = filter(keys(cfg.wireguard_servers), function(name) {
        return index(in_use, name) == -1;
      });
    }
    if (length(st.candidates) == 0) {
      log(sprintf("no more candidate servers for %s", ifname));
      continue;
    }

    // pop a random candidate off the list
    let i = math.rand() % length(st.candidates);
    let next = st.candidates[i];
    st.candidates = filter(st.candidates, (v, j) => j != i);
    st.interfaces[ifname] = { "server": next };

    log(sprintf("tunnel %s -> %s not healthy, moving to %s", ifname, current, next));

    wg_replace_endpoint(ifname, cfg, next);
  }
}

// TODO: ts_uplink interface leaks into default namespace when uplink namespace is deleted
function uplink_maintenance(nsid, netns, ifname, mode) {
  let netnsifname = UPLINK_NETNS_IFNAME;

  if (interface_exists(netnsifname)) {
    // the uplink interface will sometimes leak out of the namespace on shutdown
    shell_command("ip link set "+netnsifname+" netns "+netns);
  }

  if (interface_exists_netns(netnsifname, netns)) {
    shell_command("ip -n "+netns+" link set "+netnsifname+" up");
  } else if (!interface_exists(ifname)) {
    log(sprintf("missing uplink interface %s", ifname));
    return false;
  } else if (mode == "direct") {
    // move uplink interface directly:
    shell_command("ip link set dev "+ifname+" netns "+netns);
    shell_command("ip -n "+netns+" link set "+ifname+" name "+netnsifname);
    shell_command("ip -n "+netns+" link set "+netnsifname+" up");
  } else if (mode == "bridge") {
    // or create a macvlan bridge:
    shell_command("ip link add "+netnsifname+" link "+ifname+" type macvlan mode bridge");
    shell_command("ip link set dev "+netnsifname+" netns "+netns);
    shell_command("ip -n "+netns+" link set up "+netnsifname+"");
  } else {
    log(sprintf("uplink mode must be 'bridge' or 'direct', got '%s'", mode));
    return false;
  }

  // if we already have an IP, we'll try to renew it.
  // some routers will otherwise give us a different new IP, exhausting the IP pool.
  let p = fs.popen("ip -j -n "+netns+" a s "+netnsifname);
  let out = p.read("all");
  p.close();
  if (out == null) {
    log("unable to read current ip address of "+netnsifname)
  }
  let reqip = "0.0.0.0";
  let iplist = json(out);
  for (ipobj in iplist) {
    for (ipaddr in ipobj.addr_info) {
      if (ipaddr.family == "inet" && ipaddr.scope == "global") {
        reqip = ipaddr.local;
      }
    }
  }

  // try dhcp for 5 seconds
  shell_command("ip netns exec "+netns+" udhcpc -f -n -q -A 5 -i "+netnsifname+" -r "+reqip+" -s /usr/share/tunspace/udhcpc.script 2>&1 | grep 'ip addr add'");

  return true;
}

function boot(st, cfg) {
  debug("boot");

  if (!create_namespace(st, cfg.uplink_netns)) {
    log("failed to create "+cfg.uplink_netns+" namespace");
    exit(1);
  }
  assert(st.nsid > 0);

  for (ifname, ifcfg in cfg.wireguard_interfaces) {
    if (!create_wg_interface(st.nsid, ifname, ifcfg, cfg.uplink_netns)) {
      log("failed to create "+ifname+" interface");
      exit(1);
    }
  }

  debug("boot end");
}

function tick(st, cfg) {
  debug("tick");

  if (!uplink_maintenance(st.nsid, cfg.uplink_netns, cfg.uplink_ifname, cfg.uplink_mode)) {
    log("uplink maintenance failed");
  }
  wireguard_maintenance(st, cfg);

  uloop.timer(1000*int(cfg.maintenance_interval), () => tick(st, cfg));

  debug("tick end");
}

let state = {
  "candidates": [],
  "interfaces": {},
  "nsid": 0,
};

cfg = load_config("tunspace");

uloop.init();
boot(state, cfg);
tick(state, cfg);
uloop.run();
