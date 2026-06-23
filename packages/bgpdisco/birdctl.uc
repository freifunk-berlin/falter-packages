import * as socket from 'socket';
import { DBG, INFO, WARN, ERR } from 'bgpdisco.logger';

const RE_BABEL_NEIGHBORS = regexp('(fe80\\S*)\\s*(\\S*)', 'g');
const RE_BABEL_INTERFACES = regexp('(\\S*)\\s*(Up|Down)', 'g');

let sock_bctl_path = '/var/run/bird.ctl';
let _sock = null;
let _buf = '';

function _readline() {
  while (true) {
    let nl = index(_buf, '\n');
    if (nl >= 0) {
      let line = substr(_buf, 0, nl);
      _buf = substr(_buf, nl + 1);
      return replace(line, '\r', '');
    }
    let chunk = _sock.recv(4096);
    if (!chunk) {
      ERR('Bird socket read error: %s', _sock.error());
      return null;
    }
    _buf += chunk;
  }
}

// Read one full bird protocol response.
// Bird responses: "NNNN-text" for continuation lines, "NNNN text" for the final line.
// Returns the response body with status-code prefixes stripped, or null on error.
function _read_response() {
  let lines = [];
  while (true) {
    let line = _readline();
    if (line == null)
      return null;
    let text = replace(line, /^\d{4}[-+ ]/, '');
    if (text != '')
      push(lines, text);
    if (match(line, /^\d{4} /))
      break;
  }
  return join('\n', lines);
}

function _disconnect() {
  if (_sock) {
    _sock.close();
    _sock = null;
    _buf = '';
  }
}

function _connect() {
  _disconnect();
  DBG('Connecting to bird control socket: %s', sock_bctl_path);
  let s = socket.connect({ path: sock_bctl_path });
  if (!s) {
    ERR('Failed to connect to bird control socket %s: %s', sock_bctl_path, socket.error());
    return false;
  }
  _sock = s;
  _buf = '';
  _read_response(); // consume greeting line
  INFO('Connected to bird control socket');
  return true;
}

function cmd(command) {
  DBG('cmd(%s)', command);
  if (!_sock && !_connect())
    return '';
  if (_sock.send(command + '\n') == null) {
    WARN('Bird socket write failed, reconnecting');
    if (!_connect())
      return '';
    _sock.send(command + '\n');
  }
  let response = _read_response();
  if (response == null) {
    WARN('Bird socket read failed, will reconnect on next command');
    _disconnect();
    return '';
  }
  DBG('Response: %s', response);
  return response;
}

function get_babel_interfaces() {
  DBG('get_babel_interfaces()');
  let response = cmd('show babel interfaces');
  let matches = match(response, RE_BABEL_INTERFACES);
  let result = [];
  for (let _m in matches)
    push(result, _m[1]);
  return result;
}

function get_babel_neighbors() {
  DBG('get_babel_neighbors()');
  let response = cmd('show babel neighbors');
  let matches = match(response, RE_BABEL_NEIGHBORS);
  let result = [];
  for (let _m in matches)
    push(result, {ip: _m[1], iface: _m[2]});
  return result;
}

function init(bird_ctl) {
  DBG('init()');
  if (bird_ctl)
    sock_bctl_path = bird_ctl;
  return {
    cmd,
    get_babel_interfaces,
    get_babel_neighbors,
  };
}

export { init };
