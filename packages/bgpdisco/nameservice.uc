import * as rtnl from 'rtnl';
import * as fs from 'fs';
import * as uci from 'uci';
import { DBG, INFO, WARN, ERR } from 'bgpdisco.logger';

const PLUGIN_UID = 0;

let cfg = {
  domain: 'ff',
  hosts_file: '/var/hosts/ffnameservice',
  cmd_on_update: null,
  exclude_interface_self: []
};

let static_entries = [];

function get_interfaces_with_ip() {
  let nl_ips = rtnl.request(rtnl.const.RTM_GETADDR,
               rtnl.const.NLM_F_REQUEST|rtnl.const.NLM_F_ROOT|rtnl.const.NLM_F_MATCH,
               {scope: rtnl.const.RT_SCOPE_UNIVERSE});


  let result = {};
  for (i in nl_ips) {
    // Filter for global routable IPs
    // Scope filtering doesnt work via rtnl, idk
    if (i.scope != rtnl.const.RT_SCOPE_UNIVERSE)
      continue;

    result[split(i.address, '/')[0]] = i.dev;
  }
  return result;
}

function is_v4(ip) {
  if (substr(':'))
    return false;
  return true;
}

function get_hostname() {
  return replace(fs.readfile('/proc/sys/kernel/hostname'), '\n', '');
}


// Expects a dictionary
//  IP: [hostnames..]
function get_local_hosts() {
  let hostname = get_hostname();
  let ips = get_interfaces_with_ip();

  let lo_v4;
  let lo_v6;
  let first_v4;
  let first_v6;
  let result = {};
  for (let ip,dev in ips) {
    result[ip] ??= [];
    let name = replace(dev, '.', '_') + '.' + hostname;

    // Strip device from loopback interface first IP
    if (dev == 'lo') {
      if (!lo_v4 && is_v4)
        lo_v4 = ip;
      if (!lo_v6 && !is_v4)
        lo_v6 = ip;
    }

    push(result[ip], name);

    // Save first IP if for the case we've got no loopback
    // while ommiting interfaces which are excluded

    DBG('comparing dev=%s against exclude_interface_self=%s', dev, cfg.exclude_interface_self);
    if (dev in cfg.exclude_interface_self) {
      DBG('Excluding interface....');
      continue;
    }
    if (!first_v4 && is_v4(ip)) {
      first_v4 = ip;
      DBG('Setting first IPv4: %s, Dev: %s', ip, dev);
    }
    if (!first_v6 && !is_v4(ip)) {
      first_v6 = ip;
      DBG('Setting first IPv6: %s, Dev: %s', ip, dev);
    }
  }

  // Add records for the hostname
  push(result[lo_v4 || first_v4], hostname);
  push(result[lo_v6 || first_v6], hostname);

  // Adding records for our static entries
  for (let e in static_entries) {
    for (let ip in e.ips) {
      result[ip] ??= [];
      push(result[ip], e.host)
    }
  }
  return result;
}


// data = { ip: [hostname1, ..], }
function write_hostnames(data) {
  INFO('Writing hostnames to %s', cfg.hosts_file);
  let time = localtime();
  let fp = fs.open(cfg.hosts_file, 'w');
  for (ip in data){
    let hostnames = map(data[ip], function (v) {return v + '.' + cfg.domain;});
    fp.write(ip + ' ' + join(' ', hostnames) + '\n');
  }

  fp.write('\n' + '# Written by ffnameservice on ');
  fp.write(join('-', [time.year, time.mon, time.mday]) + ' ');
  fp.write(join(':', [time.hour, time.min, time.sec]));
  fp.close();


  if (cfg.cmd_on_update) {
    let res = system(cfg.cmd_on_update);
    INFO('Launch command on update: %s => %d', cfg.cmd_on_update, res);
  }
}


function uci_config() {
  INFO('Reading uci config');
  function handle_section(s) {
    let t = s['.type'];
    DBG('Config: handling section %s', t);
    switch (t) {
      case 'general':
        if ('domain' in s && s.domain)
          cfg.domain = s.domain;
        if ('hosts_file' in s && s.hosts_file)
          cfg.hosts_file = s.hosts_file;
        if ('cmd_on_update' in s && s.cmd_on_update)
          cfg.cmd_on_update = s.cmd_on_update;
        if ('exclude_interface_self' in s)
          if (type(s.exclude_interface_self) != 'array') {
            ERR('Config exclude_interface_self is not a list - Ignore');
            return;
          }
          cfg.exclude_interface_self = s.exclude_interface_self;
        break;
      case 'static-entry':
        INFO('Loading static host entry - Host: %s, IPs: %s', s.host, s.ip);
        if (s.host == null || type(s.ip) != 'array') {
          ERR('Error while reading static entry - Skip');
          return;
        }
        push(static_entries, { host: s.host, ips: s.ip});
        break;
      default:
        ERR('Ignoring unknown section "%s" while parsing configuration', t);
    }
  }
  let ctx = uci.cursor();
  ctx.foreach('bgpdisco_nameservice', null, handle_section);
}

return {
  init: function (plug) {
    uci_config();
    plug.register(plug.TYPE.DATA_PROVIDER, PLUGIN_UID, get_local_hosts);
    plug.register(plug.TYPE.DATA_HANDLER, PLUGIN_UID, write_hostnames);
  }
};
