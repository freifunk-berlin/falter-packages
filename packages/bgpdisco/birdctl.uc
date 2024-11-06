import {popen} from 'fs';
import { DBG, INFO, WARN, ERR } from 'bgpdisco.logger';

let sock_bctl_path = '/var/run/bird.ctl';
const BIRDC_PATH = '/usr/sbin/birdc';
const RE_BABEL_NEIGHBORS = regexp('(fe80\\S*)\\s*(\\S*)', 'g');
const RE_BABEL_INTERFACES = regexp('(\\S*)\\s*(Up|Down)', 'g');

function cmd(cmd) {
  DBG('cmd()');
  let escaped_command = replace(cmd,'"','\\\"');

  let cmd_string = sprintf('%s -s %s %s', BIRDC_PATH, sock_bctl_path, escaped_command);
  DBG('Popen: %s', cmd_string);
  let p = popen(cmd_string);
  let response = p.read('all');
  DBG('Output: %s', response);
  p.close();

  return response;
}

function get_babel_interfaces() {
  DBG('get_babel_interfaces()');
  let response = cmd('show babel interfaces');
  let matches = match(response, RE_BABEL_INTERFACES); // returns [[iface0, Up], [iface1.., Down]]
  let result = [];
  DBG('matches: %J', matches);
  for (let _m in matches) {
    push(result, _m[1]);
  }
  return result;
}

function get_babel_neighbors() {
  DBG('get_babel_neighbors()');
  let response = cmd('show babel neighbors');
  let matches = match(response, RE_BABEL_NEIGHBORS); // returns [[fe80.., iface], [fe80.., iface]]
  let result = [];
  for (let _m in matches) {
    push(result, {ip: _m[1], iface: _m[2]});
  }
  return result;
}

function init(bird_ctl) {
  DBG('init()');

  if (bird_ctl) {
    sock_bctl_path = bird_ctl;
  }
  return {
    cmd: cmd,
    get_babel_interfaces: get_babel_interfaces,
    get_babel_neighbors: get_babel_neighbors
  };
}

export { init };
