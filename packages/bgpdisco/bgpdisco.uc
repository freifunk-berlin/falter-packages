#!/usr/bin/ucode

import * as rtnl from 'rtnl';
import * as uloop from 'uloop';
import * as fs from 'fs';
import * as uci from 'uci';
import * as mrtdump from 'bgpdisco.mrtdump';
import * as plugin from 'bgpdisco.plugin';
import * as birdctl from 'bgpdisco.birdctl';
import { DBG, INFO, WARN, ERR, enable_debug } from 'bgpdisco.logger';

let tty;
let bird;
let plugins;

let monitor_interfaces = [];

let cache_neighbors;
let cache_data;

let timer_refresh_remote_data;

let cfg = {
    // General
    general_debug: false,
    general_plugin_directory: '/usr/share/ucode/bgpdisco/plugins/',
    general_refresh_remote_data_interval: 60,
    // Bird
    bird_control_socket: '/var/run/bird.ctl',
    bird_config_target: '/dev/shm/bird_bgpdisco.conf',
    bird_config_template: '/usr/share/ucode/bgpdisco/bird_config_template.ut',
    bird_mrt_file: '/tmp/mrt_bgpdisco.dump',
};

function render_bird_config() {
  DBG('render_bird_config()');
  return render(cfg.bird_config_template, proto({
                index: index, hexenc, replace,
                neighbors: cache_neighbors, data: plugins.provide_data()
                }, {}));
}

function configure_bird() {
  DBG('configure_bird()');

  let config = render_bird_config();

  DBG('write rendered config');
  if (!fs.writefile(cfg.bird_config_target, config)) {
    ERR('cant write bird config to filesystem');
  }

  INFO('triggering bird config reload');
  bird.cmd('configure');
}

function retrieve_data_from_bird() {
  DBG('retrieve_data_from_bird()');

  DBG('remove old mrt dump');
  fs.unlink(cfg.bird_mrt_file);

  DBG('request new mrt dump');
  bird.cmd('mrt dump table "*_bgpdisco" to "' + cfg.bird_mrt_file + '"');


  DBG('parse mrt dump');
  let data = mrtdump.get_routes(cfg.bird_mrt_file);

  DBG('processing routes');
  let parsed_data = {};
  for (let route in data) {
    if (index(keys(route.attributes), '250') == -1) {
      DBG('skip route due to missing magic attribute: %s', route);
      continue;
    }
    let ip = split(route.prefix, '/')[0];
    for (let darr in json(route.attributes['250'])) {
      let id = shift(darr);
      parsed_data[id] ??= {};
      if (index(keys(parsed_data[id]), ip) != -1)
        DBG('route is already parsed, do we have stale routes in the network?: %s', route);
      parsed_data[id][ip] = darr;
    }
  }
  return parsed_data;
}

function sync_peers() {
  DBG('sync_peers()');
  let neighbors = bird.get_babel_neighbors();
  if (sprintf('%s', neighbors) == sprintf('%s', cache_neighbors)) {
    DBG('No change in babel neighbors - no sync required');
    return;
  }
  INFO('Babel neighbors have changed - sync to BGP');
  cache_neighbors = neighbors;
  configure_bird();
}

function cb_refresh_remote_data() {
  DBG('cb_refresh_remote_data()');
  let data = retrieve_data_from_bird();
  if (sprintf('%s', data) == sprintf('%s', cache_data)) {
    DBG('Received data matches cache - no need to trigger handler plugins');
    return;
  };
  INFO('Received data differs from cache - trigger handler plugins');
  cache_data = data;
  plugins.handle_data(data);
}

function trigger_refresh_remote_data() {
  cb_refresh_remote_data();
  timer_refresh_remote_data.set(cfg.general_refresh_remote_data_interval*1000);
}

function cb_nl_newneigh(ev) {
  DBG('cb_nl_newneigh(msg.dev=%s)', ev.msg.dev);
  // Not necessary, coz we filter on listener registration
  //  if (ev.cmd != rtnl.const.RTM_NEWNEIGH):
  //    log('RTM_ADDNEIGH');
  //  }

  // Ignore other Families
  if (ev.msg.family != rtnl.const.AF_INET6) {
    return;
  }

  // Ignore other interfaces
  if (length(cfg.neighbor_sync_monitor_interfaces) > 0) {
    if (!(ev.msg.dev in cfg.neighbor_sync_monitor_interfaces))
      return;
  }

  // Ignore other state changes than reachable
  if (ev.msg.state != rtnl.const.NUD_REACHABLE) {
    return;
  }

  // Ignore other IPs than link local
  if (substr(ev.msg.dst, 0, 4) != 'fe80') {
    return;
  }

  DBG('Learned new neighbor - triggering peer syncronization. IP: %s, Dev:', ev.msg.dst, ev.msg.dev);
  sync_peers();
}

function uci_config() {
  function string(val) {
    return val;
  }
  function bool(val) {
    return int(val) == 1;
  }

  function OPT(section, option, type) {
    let name = section['.type'];
    let cfg_val = section[option];
    if (option in section) {
      let val = call(type, null, null, cfg_val);
      DBG("Config: Reading option %s - %s with value %s -> %s", option, name, cfg_val, val);
      cfg[name + '_' + option] = call(type, null, null, section[option]);
    }
  }
  function handle_section(s) {
    let t = s['.type'];
    DBG('Config: handling section %s', t);
    switch (t) {
      case 'general':
        OPT(s, 'debug', bool);
        OPT(s, 'refresh_remote_data_interval', int);
        break;
      case 'bird':
        OPT(s, 'control_socket', string);
        OPT(s, 'config_target', string);
        OPT(s, 'config_template', string);
        OPT(s, 'mrt_file', string);
        break;
      default:
        ERR('Ignoring unknown section "%s" while parsing configuration', t);
    }
  }
  let ctx = uci.cursor();
  ctx.foreach('bgpdisco', null, handle_section);
}

INFO('Start');

uci_config();

if (cfg.general_debug)
  enable_debug();

bird = birdctl.init(cfg.bird_control_socket);

plugins = plugin.init(cfg.general_plugin_directory);

// setup refresh data timer with initial timer of 5s, to let the peers get syncronized before
timer_refresh_remote_data = uloop.timer(5000, trigger_refresh_remote_data);

monitor_interfaces = bird.get_babel_interfaces();
INFO('Monitoring following interfaces: %s', monitor_interfaces);

if (length(monitor_interfaces) == 0)
  WARN('Warning, couldnt retrieve babel interfacs from bird. Listening on all interfaces');

INFO('Enabling monitoring for new neighbors');
rtnl.listener(cb_nl_newneigh, [rtnl.const.RTM_NEWNEIGH], [rtnl.const.RTNLGRP_NEIGH], {});

// sync peers right away
sync_peers();

// get the party started :)
uloop.run();
