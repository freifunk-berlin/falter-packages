#!/usr/bin/ucode

import * as rtnl from 'rtnl';
import * as uloop from 'uloop';
import * as fs from 'fs';
import * as uci from 'uci';
import * as bgp from 'bgpdisco.bgp';
import * as plugin from 'bgpdisco.plugin';
import * as birdctl from 'bgpdisco.birdctl';
import { DBG, INFO, WARN, ERR, enable_debug } from 'bgpdisco.logger';

// How long handlers wait after the first change, to take a burst in one run
const HANDLER_DEBOUNCE_MS = 1000;
// How long we wait for bird to send its routes before we use what we have
const SYNC_TIMEOUT_MS = 15000;
// How long data of an earlier session is kept when bird did not send it again.
// After its own restart bird reports its table as complete while its sessions
// into the mesh are still coming up.
const STALE_GRACE_MS = 60000;

let tty;
let bird;
let bird_bgp;
let plugins;

let monitor_interfaces = [];

// How often we look for babel neighbors after a link came up, and how many times
const FAST_PEER_SYNC_MS = 1000;
const FAST_PEER_SYNCS = 30;

// not in the constants of the rtnl module
const IFF_UP = 0x1;
const IFF_LOWER_UP = 0x10000;

let cache_neighbors;
// Interfaces that are up, and the fast comparisons that are left
let links_up = {};
let fast_peer_syncs = 0;
// A neighbor that babel has lost keeps its session for a while, see sync_peers()
let peer_removal_pending = false;
let peer_removal_due = false;

// ip -> attribute 250: what the plugins provide, and what bird sent us
let local_data = {};
let remote_data = {};
// IPs in remote_data that bird did not send again in the current session
let stale = {};
// bird has sent its whole table in the current session
let rib_complete = false;
// The grace period of the stale data is running / is over, but the table of
// bird was not complete then. One period for all of it: a reconnect does not
// start it again, or data would never expire while the session flaps.
let stale_grace_running = false;
let stale_grace_over = false;

// While bird is still sending its table, handlers keep their last result
let syncing = true;
let synced_families = {};
let handlers_pending = false;
// Monotonic time in ms of the last handler run
let handlers_last_run = null;

// uloop only frees a timer that is cancelled, and a cancelled one cannot be
// set again: each of these is created once and then only ever set.
let timer_refresh_local_data;
let timer_sync_debounce;
let timer_sync_peers;
let timer_peer_removal;
let timer_handlers;
let timer_sync;
let timer_stale;

let cfg = {
    // General
    general_debug: false,
    general_plugin_directory: '/usr/share/ucode/bgpdisco/plugins/',
    general_refresh_local_data_interval: 60,
    general_handler_min_interval: 30,
    general_peer_sync_interval: 30,
    general_peer_removal_delay: 300,
    // Bird
    bird_control_socket: '/var/run/bird.ctl',
    bird_config_target: '/dev/shm/bird_bgpdisco.conf',
    bird_config_template: '/usr/share/ucode/bgpdisco/bird_config_template.ut',
    bird_bgp_address: '::1',
    bird_bgp_port: 179,
    bird_router_id: null,
    bird_graceful_restart_time: 120,
    bird_vrf: null,
};

function render_bird_config() {
  DBG('render_bird_config()');
  return render(cfg.bird_config_template, proto({
                index: index, replace,
                neighbors: cache_neighbors, bgp_address: cfg.bird_bgp_address,
                vrf: cfg.bird_vrf
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

function now_ms() {
  let t = clock(true);
  return t[0] * 1000 + int(t[1] / 1000000);
}

function arm(timer, ms, cb) {
  if (!timer)
    return uloop.timer(ms, cb);
  timer.set(ms);
  return timer;
}

function run_handlers() {
  handlers_pending = false;
  if (syncing || !plugins.has_data_handlers)
    return;

  DBG('run_handlers()');
  let parsed_data = {};
  function add(ip, attr) {
    let records;
    try {
      records = json(attr);
    } catch (e) {
      DBG('Ignoring malformed data of %s: %s', ip, e);
      return;
    }
    if (type(records) != 'array')
      return;
    for (let darr in records) {
      if (type(darr) != 'array')
        continue;
      let id = shift(darr);
      parsed_data[id] ??= {};
      parsed_data[id][ip] = darr;
    }
  }

  // bird does not send our own routes back to us, and our IPs are ours
  for (let ip, attr in remote_data)
    if (!(ip in local_data))
      add(ip, attr);
  for (let ip, attr in local_data)
    add(ip, attr);

  handlers_last_run = now_ms();
  plugins.handle_data(parsed_data);
}

function cb_handlers() {
  if (handlers_pending)
    run_handlers();
}

// Handlers run a moment after the first change, but not more often than once
// per handler_min_interval: a burst of changes ends up in one run.
function data_changed() {
  if (syncing || handlers_pending || !plugins.has_data_handlers)
    return;
  handlers_pending = true;

  let wait = HANDLER_DEBOUNCE_MS;
  if (handlers_last_run != null) {
    let remaining = handlers_last_run + cfg.general_handler_min_interval * 1000 - now_ms();
    if (remaining > wait)
      wait = remaining;
  }
  timer_handlers = arm(timer_handlers, wait, cb_handlers);
}

function purge_stale() {
  stale_grace_over = false;
  if (length(stale) == 0)
    return;
  INFO('Dropping data of %d IPs that bird did not send again', length(stale));
  for (let ip in stale)
    delete remote_data[ip];
  stale = {};
  data_changed();
}

function cb_stale() {
  stale_grace_running = false;
  if (length(stale) == 0)
    return;
  // Only a complete table tells what is gone
  if (rib_complete)
    purge_stale();
  else
    stale_grace_over = true;
}

function sync_done() {
  if (!syncing)
    return;
  DBG('sync_done()');
  syncing = false;
  data_changed();
}

// bird has sent everything it has: what is still stale now is on its way out
function rib_completed() {
  rib_complete = true;
  if (stale_grace_over) {
    purge_stale();
  } else if (length(stale) > 0 && !stale_grace_running) {
    stale_grace_running = true;
    timer_stale = arm(timer_stale, STALE_GRACE_MS, cb_stale);
  }
}

function cb_sync_timeout() {
  // The timer may be left over from a session that is gone
  if (!syncing || !bird_bgp.established())
    return;
  WARN('bird did not finish sending its routes - using what we have');
  sync_done();
}

function refresh_local_data() {
  DBG('refresh_local_data()');
  let data = {};
  let provided = plugins.provide_data();
  for (let ip in provided) {
    let addr = iptoarr(ip);
    if (!addr) {
      WARN('Plugin provided data for "%s", which is not an IP - ignored', ip);
      continue;
    }
    data[arrtoip(addr)] = sprintf('%J', provided[ip]);
  }

  let changed = false;
  for (let ip, attr in data) {
    if (local_data[ip] == attr)
      continue;
    changed = true;
    bird_bgp.announce(ip, attr);
  }
  for (let ip in local_data) {
    if (ip in data)
      continue;
    changed = true;
    bird_bgp.withdraw(ip);
  }
  local_data = data;

  if (changed) {
    INFO('Local data has changed - announcing %d IPs', length(local_data));
    data_changed();
  }
}

function trigger_refresh_local_data() {
  refresh_local_data();
  timer_refresh_local_data.set(cfg.general_refresh_local_data_interval * 1000);
}

function cb_bgp_up() {
  DBG('cb_bgp_up()');
  // bird sends its whole table now. What we have from an earlier session
  // stays until bird had the time to learn it again.
  stale = {};
  for (let ip in remote_data)
    stale[ip] = true;
  synced_families = {};
  rib_complete = false;
  syncing = true;
  timer_sync = arm(timer_sync, SYNC_TIMEOUT_MS, cb_sync_timeout);

  for (let ip, attr in local_data)
    bird_bgp.announce(ip, attr);
  bird_bgp.end_of_rib();
}

function cb_bgp_down() {
  DBG('cb_bgp_down()');
  // Keep what we know until bird is back
  syncing = true;
  rib_complete = false;
}

function cb_bgp_route(ip, attr) {
  delete stale[ip];
  if (attr == null) {
    if (!(ip in remote_data))
      return;
    delete remote_data[ip];
  } else {
    if (remote_data[ip] == attr)
      return;
    remote_data[ip] = attr;
  }
  data_changed();
}

function cb_bgp_end_of_rib(family) {
  DBG('cb_bgp_end_of_rib(%d)', family);
  synced_families[family] = true;
  if (synced_families[4] && synced_families[6] && !rib_complete) {
    sync_done();
    rib_completed();
  }
}

function neighbor_key(n) {
  return n.iface + '/' + n.ip;
}

// Makes the BGP sessions follow the babel neighbors of bird.
//
// A new neighbor gets its session at once. One that babel has lost keeps it
// for peer_removal_delay seconds: on a bad link babel loses a neighbor long
// before the BGP session is gone, and every removal is a reconfiguration of
// bird and a new session later. One delay for all of them: it starts with
// the first neighbor that is missing, and takes what is missing when it ends.
function sync_peers() {
  DBG('sync_peers()');
  let neighbors = bird.get_babel_neighbors();
  if (neighbors == null) {
    WARN('Could not get the babel neighbors from bird - keeping the BGP sessions as they are');
    return;
  }

  let wanted = {};
  for (let n in neighbors)
    wanted[neighbor_key(n)] = n;

  let added = 0, lost = 0;
  for (let n in cache_neighbors) {
    let key = neighbor_key(n);
    if (key in wanted)
      continue;
    lost++;
    if (!peer_removal_due)
      wanted[key] = n;
  }
  let known = {};
  for (let n in cache_neighbors)
    known[neighbor_key(n)] = true;
  for (let key in wanted)
    if (!(key in known))
      added++;

  if (lost > 0 && !peer_removal_due && !peer_removal_pending) {
    DBG('%d babel neighbors are gone - their sessions stay for %d s', lost, cfg.general_peer_removal_delay);
    peer_removal_pending = true;
    timer_peer_removal = arm(timer_peer_removal, cfg.general_peer_removal_delay * 1000, function() {
      peer_removal_pending = false;
      peer_removal_due = true;
      sync_peers();
      peer_removal_due = false;
    });
  }

  let removed = peer_removal_due ? lost : 0;
  if (cache_neighbors != null && added == 0 && removed == 0) {
    DBG('No change in babel neighbors - no sync required');
    return;
  }

  INFO('Babel neighbors have changed (%d new, %d removed) - sync to BGP', added, removed);
  // sorted, so that the config only changes when the neighbors do
  cache_neighbors = map(sort(keys(wanted)), key => wanted[key]);
  configure_bird();
}

// The kernel only tells us about neighbors it resolves. Tunnels have none
// (wireguard is NOARP), and interfaces that show up later are not in
// monitor_interfaces: the neighbor events only make it faster, this is what
// makes sure that every babel neighbor gets its session.
function trigger_sync_peers() {
  sync_peers();
  if (fast_peer_syncs > 0) {
    fast_peer_syncs--;
    timer_sync_peers.set(FAST_PEER_SYNC_MS);
  } else {
    timer_sync_peers.set(cfg.general_peer_sync_interval * 1000);
  }
}

function link_is_up(flags) {
  // an event has the flags together with the mask of the changed ones
  if (type(flags) == 'array')
    flags = flags[0];
  return (flags & IFF_UP) && (flags & IFF_LOWER_UP);
}

// A link that came up is where a new neighbor shows up, a tunnel most of
// all. bird has it once the first hello is in: look every second for a while.
function cb_nl_link(ev) {
  let dev = ev.msg.ifname;
  if (ev.cmd == rtnl.const.RTM_DELLINK || !link_is_up(ev.msg.flags)) {
    delete links_up[dev];
    return;
  }
  if (links_up[dev])
    return;
  links_up[dev] = true;

  DBG('Link %s came up - looking for new neighbors every second', dev);
  fast_peer_syncs = FAST_PEER_SYNCS;
  if (timer_sync_peers)
    timer_sync_peers.set(FAST_PEER_SYNC_MS);
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

  // Ignore interfaces not managed by babel
  if (length(monitor_interfaces) > 0) {
    if (!(ev.msg.dev in monitor_interfaces))
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

  DBG('Learned new neighbor - scheduling peer sync. IP: %s, Dev: %s', ev.msg.dst, ev.msg.dev);
  timer_sync_debounce = arm(timer_sync_debounce, 500, sync_peers);
}

function uci_config() {
  function string(val) {
    return val;
  }
  function bool(val) {
    return int(val) == 1;
  }
  // int() gives NaN for anything else, and NaN slips through every comparison
  function number(val) {
    let n = int(val);
    return (type(n) == 'int' && n >= 0) ? n : null;
  }

  function OPT(section, option, type) {
    let name = section['.type'];
    let cfg_val = section[option];
    if (option in section) {
      let val = call(type, null, null, cfg_val);
      if (val == null) {
        ERR('Config: invalid value "%s" for option %s - %s, keeping the default', cfg_val, option, name);
        return;
      }
      DBG("Config: Reading option %s - %s with value %s -> %s", option, name, cfg_val, val);
      cfg[name + '_' + option] = val;
    }
  }
  function handle_section(s) {
    let t = s['.type'];
    DBG('Config: handling section %s', t);
    switch (t) {
      case 'general':
        OPT(s, 'debug', bool);
        OPT(s, 'refresh_local_data_interval', number);
        OPT(s, 'handler_min_interval', number);
        OPT(s, 'peer_sync_interval', number);
        OPT(s, 'peer_removal_delay', number);
        break;
      case 'bird':
        OPT(s, 'control_socket', string);
        OPT(s, 'config_target', string);
        OPT(s, 'config_template', string);
        OPT(s, 'bgp_address', string);
        OPT(s, 'bgp_port', number);
        OPT(s, 'router_id', string);
        OPT(s, 'graceful_restart_time', number);
        OPT(s, 'vrf', string);
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

bird_bgp = bgp.init({
  host: cfg.bird_bgp_address,
  port: cfg.bird_bgp_port,
  router_id: cfg.bird_router_id,
  graceful_restart_time: cfg.bird_graceful_restart_time,
  on_up: cb_bgp_up,
  on_down: cb_bgp_down,
  on_route: cb_bgp_route,
  on_end_of_rib: cb_bgp_end_of_rib,
});

if (!plugins.has_data_handlers)
  INFO('No data handlers registered - remote data is not processed');

monitor_interfaces = bird.get_babel_interfaces();
INFO('Monitoring following interfaces: %s', monitor_interfaces);

if (length(monitor_interfaces) == 0)
  WARN('Warning, couldnt retrieve babel interfacs from bird. Listening on all interfaces');

INFO('Enabling monitoring for new neighbors');
rtnl.listener(cb_nl_newneigh, [rtnl.const.RTM_NEWNEIGH], [rtnl.const.RTNLGRP_NEIGH], {});

// sync peers right away, this also configures our own session in bird
sync_peers();
if (cache_neighbors == null) {
  // bird did not answer: write the config without neighbors for now
  cache_neighbors = [];
  configure_bird();
}
timer_sync_peers = uloop.timer(cfg.general_peer_sync_interval * 1000, trigger_sync_peers);

for (let link in rtnl.request(rtnl.const.RTM_GETLINK, rtnl.const.NLM_F_DUMP, {}))
  if (link_is_up(link.flags))
    links_up[link.ifname] = true;
rtnl.listener(cb_nl_link, [rtnl.const.RTM_NEWLINK, rtnl.const.RTM_DELLINK], [rtnl.const.RTNLGRP_LINK], {});

refresh_local_data();
timer_refresh_local_data = uloop.timer(cfg.general_refresh_local_data_interval * 1000, trigger_refresh_local_data);

bird_bgp.start();

// get the party started :)
uloop.run();
