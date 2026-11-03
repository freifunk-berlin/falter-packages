// Minimal BGP speaker for the loopback session between bgpdisco and bird.
//
// It is not a router: no path selection, no policy, one peer. bird does all
// of that. This end only
//   - announces and withdraws host routes carrying attribute 250,
//   - reports the host routes bird sends, with their attribute 250,
//   - keeps the session alive and reconnects when it is lost.
// Anything unexpected closes the session; bird resends everything after the
// reconnect.
//
// The session is only ever closed on the TCP level, we never send a
// NOTIFICATION: bird answers one by dropping our routes at once and refusing
// new connections for its error wait time (60 to 300 s). After a plain close
// it keeps the routes (graceful restart) and takes the next connection right
// away.
//
// On loopback we trust bird: the marker is not checked and unknown message
// types are skipped.

import * as socket from 'socket';
import * as uloop from 'uloop';
import { DBG, INFO, WARN, ERR } from 'bgpdisco.logger';

const MSG_OPEN = 1;
const MSG_UPDATE = 2;
const MSG_NOTIFICATION = 3;  // only received, see above
const MSG_KEEPALIVE = 4;

const HEADER_LEN = 19;
const MAX_MSG_LEN = 4096;

const ATTR_ORIGIN = 1;
const ATTR_AS_PATH = 2;
const ATTR_NEXT_HOP = 3;
const ATTR_LOCAL_PREF = 5;
const ATTR_MP_REACH = 14;
const ATTR_MP_UNREACH = 15;
const ATTR_BGPDISCO = 250;

const CAP_MP = 1;
const CAP_GRACEFUL_RESTART = 64;
const CAP_AS4 = 65;

const AS_TRANS = 23456;

// The restart time has 12 bits in the capability
const MAX_GRACEFUL_RESTART_TIME = 4095;
// What bird gets after its hold time ran out with data waiting in the socket
const HOLD_GRACE_MS = 10000;
// A session that fails after the OPEN of bird and before it was up for this
// long is retried slower and slower: bird does not slow us down, as we never
// send it a NOTIFICATION
const STABLE_SESSION_MS = 60000;
const MAX_RECONNECT_INTERVAL = 300;

// The routes are never used for forwarding, the next hops are documentation
// addresses, same as in the bird config template.
const NEXT_HOP_V4 = [192, 0, 2, 0];
const NEXT_HOP_V6 = [0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];

const MARKER = chr(255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255);

const ST_IDLE = 0;         // no connection
const ST_CONNECTED = 1;    // waiting for the OPEN of bird
const ST_OPENCONFIRM = 2;  // our OPEN is out, waiting for the first KEEPALIVE
const ST_ESTABLISHED = 3;

let opts = {
  host: '::1',
  port: 179,
  router_id: null,           // default: derived from the router ID of bird
  hold_time: 90,
  graceful_restart_time: 120,
  reconnect_interval: 5,
  on_up: null,               // ()
  on_down: null,             // ()
  on_route: null,            // (ip, attribute 250 or null for a withdraw)
  on_end_of_rib: null,       // (4 or 6)
};

let state = ST_IDLE;
let sock = null;
let handle = null;
let rx = '';
let hold_time = 0;
let hold_grace_used = false;
// Until the first session is up, our OPEN says that we have restarted
let restarted = true;

// A cancelled uloop timer cannot be set again, and one that is not cancelled
// is never freed. So these two are created per connection and cancelled with
// it, the reconnect timer is created once and only ever set.
let timer_hold;
let timer_keepalive;
let timer_reconnect;
// Seconds until the next attempt after a session that was not stable
let reconnect_backoff = 0;
let established_at = 0;

let connect;

function u16(n) {
  return chr((n >> 8) & 0xff, n & 0xff);
}

function u32(n) {
  return chr((n >> 24) & 0xff, (n >> 16) & 0xff, (n >> 8) & 0xff, n & 0xff);
}

function r16(buf, o) {
  return (ord(buf, o) << 8) | ord(buf, o + 1);
}

function r32(buf, o) {
  return (ord(buf, o) << 24) | (ord(buf, o + 1) << 16) | (ord(buf, o + 2) << 8) | ord(buf, o + 3);
}

function attribute(flags, type, value) {
  let len = length(value);
  if (len > 255)
    return chr(flags | 0x10, type) + u16(len) + value;
  return chr(flags, type, len) + value;
}

function now_ms() {
  let t = clock(true);
  return t[0] * 1000 + int(t[1] / 1000000);
}

function schedule_reconnect(seconds) {
  if (!timer_reconnect)
    timer_reconnect = uloop.timer(seconds * 1000, connect);
  else
    timer_reconnect.set(seconds * 1000);
}

function down(reason) {
  if (state == ST_IDLE)
    return;

  let was_established = (state == ST_ESTABLISHED);
  // Without its OPEN bird was just not ready for us: no reason to slow down
  let failed = (state != ST_CONNECTED)
            && !(was_established && now_ms() - established_at >= STABLE_SESSION_MS);
  let delay = opts.reconnect_interval;
  if (failed) {
    if (reconnect_backoff > delay)
      delay = reconnect_backoff;
    reconnect_backoff = (delay * 2 > MAX_RECONNECT_INTERVAL) ? MAX_RECONNECT_INTERVAL : delay * 2;
  } else {
    reconnect_backoff = 0;
  }
  WARN('BGP session to bird closed: %s - next attempt in %d s', reason, delay);

  if (handle) {
    handle.delete();
    handle = null;
  }
  if (sock) {
    sock.close();
    sock = null;
  }
  rx = '';
  state = ST_IDLE;
  if (timer_hold) {
    timer_hold.cancel();
    timer_hold = null;
  }
  if (timer_keepalive) {
    timer_keepalive.cancel();
    timer_keepalive = null;
  }
  schedule_reconnect(delay);

  if (was_established && opts.on_down)
    opts.on_down();
}

function send(type, body) {
  if (!sock)
    return false;
  let msg = MARKER + u16(HEADER_LEN + length(body)) + chr(type) + body;
  if (sock.send(msg) != length(msg)) {
    down(sprintf('send failed: %s', sock.error()));
    return false;
  }
  return true;
}

function send_open(peer_as, peer_id) {
  let id = opts.router_id;
  if (id == peer_id) {
    WARN('Configured router ID is the one of bird - deriving it instead');
    id = null;
  }
  if (id == null) {
    // A router ID out of 240.0.0.0/4, which no bird in the mesh uses. It ends
    // up as ORIGINATOR_ID of our routes. bird only refuses its own ID there,
    // so all that matters is that it differs from the ID of the local bird.
    id = (peer_id & 0x0fffffff) | 0xf0000000;
    if (id == peer_id)
      id ^= 1;
  }

  // R bit, only after our own start (RFC 4724): we did not keep state over
  // the restart, bird does not wait for our end-of-RIB in its own recovery.
  // F bit per family: bird keeps our routes while we are away.
  let gr = u16((restarted ? 0x8000 : 0) | opts.graceful_restart_time)
         + chr(0, 1, 1, 0x80)
         + chr(0, 2, 1, 0x80);

  let caps = chr(CAP_MP, 4, 0, 1, 0, 1)
           + chr(CAP_MP, 4, 0, 2, 0, 1)
           + chr(CAP_AS4, 4) + u32(peer_as)
           + chr(CAP_GRACEFUL_RESTART, length(gr)) + gr;
  let params = chr(2, length(caps)) + caps;

  DBG('Sending OPEN: AS %d, router ID %s', peer_as, arrtoip([(id >> 24) & 0xff, (id >> 16) & 0xff, (id >> 8) & 0xff, id & 0xff]));

  return send(MSG_OPEN, chr(4) + u16(peer_as > 0xffff ? AS_TRANS : peer_as) + u16(opts.hold_time)
                        + u32(id) + chr(length(params)) + params);
}

function rx_open(buf, o, end) {
  if (state != ST_CONNECTED)
    return down('unexpected OPEN');
  if (end - o < HEADER_LEN + 10 || ord(buf, o + 19) != 4)
    return down('malformed OPEN');

  let peer_as = r16(buf, o + 20);
  let peer_hold = r16(buf, o + 22);
  let peer_id = r32(buf, o + 24);

  // Look for the four-octet AS number in the capabilities
  let p = o + 29;
  let pend = p + ord(buf, o + 28);
  if (pend > end)
    return down('malformed OPEN');
  while (p + 2 <= pend) {
    let ptype = ord(buf, p), plen = ord(buf, p + 1);
    let c = p + 2, cend = c + plen;
    p = cend;
    if (ptype != 2 || cend > pend)
      continue;
    while (c + 2 <= cend) {
      let code = ord(buf, c), clen = ord(buf, c + 1);
      if (code == CAP_AS4 && clen == 4 && c + 6 <= cend)
        peer_as = r32(buf, c + 2);
      c += 2 + clen;
    }
  }

  hold_time = (peer_hold < opts.hold_time) ? peer_hold : opts.hold_time;
  if (hold_time > 0 && hold_time < 3)
    return down('unacceptable hold time');
  if (hold_time == 0) {
    timer_hold.cancel();
    timer_hold = null;
  }

  // Internal session: we take the AS of bird
  if (!send_open(peer_as, peer_id) || !send(MSG_KEEPALIVE, ''))
    return;
  state = ST_OPENCONFIRM;
}

// Walks a list of prefixes. Only host routes are reported, bgpdisco does not
// use anything else.
function rx_nlri(buf, o, end, v6, data) {
  let alen = v6 ? 16 : 4;
  while (o < end) {
    let plen = ord(buf, o);
    let nb = (plen + 7) >> 3;
    if (nb > alen || o + 1 + nb > end)
      return false;
    if (plen == alen * 8) {
      let addr = [];
      for (let i = 1; i <= alen; i++)
        push(addr, ord(buf, o + i));
      opts.on_route(arrtoip(addr), data);
    }
    o += 1 + nb;
  }
  return true;
}

function rx_update(buf, o, end) {
  if (state != ST_ESTABLISHED)
    return down('unexpected UPDATE');

  let p = o + HEADER_LEN;
  if (p + 4 > end)
    return down('malformed UPDATE');

  // Withdrawn IPv4 routes
  let wlen = r16(buf, p);
  p += 2;
  if (p + wlen + 2 > end || !rx_nlri(buf, p, p + wlen, false, null))
    return down('malformed UPDATE');
  p += wlen;

  let alen = r16(buf, p);
  p += 2;
  let aend = p + alen;
  if (aend > end)
    return down('malformed UPDATE');

  if (wlen == 0 && alen == 0 && aend == end) {
    opts.on_end_of_rib(4);
    return;
  }

  let data = null;
  let reach = null;
  while (p < aend) {
    if (p + 3 > aend)
      return down('malformed attribute list');
    let flags = ord(buf, p), type = ord(buf, p + 1), len;
    if (flags & 0x10) {
      if (p + 4 > aend)
        return down('malformed attribute list');
      len = r16(buf, p + 2);
      p += 4;
    } else {
      len = ord(buf, p + 2);
      p += 3;
    }
    if (p + len > aend)
      return down('malformed attribute list');

    if (type == ATTR_BGPDISCO) {
      data = substr(buf, p, len);
    } else if (type == ATTR_MP_REACH) {
      // AFI, SAFI, next hop length, next hop, reserved byte, prefixes
      if (len < 5 || 5 + ord(buf, p + 3) > len)
        return down('malformed MP_REACH_NLRI');
      reach = [r16(buf, p), ord(buf, p + 2), p + 5 + ord(buf, p + 3), p + len];
    } else if (type == ATTR_MP_UNREACH) {
      if (len < 3)
        return down('malformed MP_UNREACH_NLRI');
      let afi = r16(buf, p), safi = ord(buf, p + 2);
      if (safi == 1 && (afi == 1 || afi == 2)) {
        if (len == 3)
          opts.on_end_of_rib(afi == 2 ? 6 : 4);
        else if (!rx_nlri(buf, p + 3, p + len, afi == 2, null))
          return down('malformed MP_UNREACH_NLRI');
      }
    }
    p += len;
  }

  // A route without our attribute carries no data: same as a withdraw
  if (reach && reach[1] == 1 && (reach[0] == 1 || reach[0] == 2))
    if (!rx_nlri(buf, reach[2], reach[3], reach[0] == 2, data))
      return down('malformed MP_REACH_NLRI');
  if (aend < end && !rx_nlri(buf, aend, end, false, data))
    return down('malformed UPDATE');
}

function cb_keepalive() {
  if (send(MSG_KEEPALIVE, ''))
    timer_keepalive.set(hold_time * 1000 / 3);
}

function rx_message(buf, o, end) {
  let type = ord(buf, o + 18);

  hold_grace_used = false;
  if (timer_hold)
    timer_hold.set(hold_time * 1000);

  switch (type) {
    case MSG_OPEN:
      rx_open(buf, o, end);
      break;
    case MSG_UPDATE:
      rx_update(buf, o, end);
      break;
    case MSG_NOTIFICATION:
      down(sprintf('NOTIFICATION from bird, code %d subcode %d',
                   (end - o > HEADER_LEN) ? ord(buf, o + 19) : 0,
                   (end - o > HEADER_LEN + 1) ? ord(buf, o + 20) : 0));
      break;
    case MSG_KEEPALIVE:
      if (state == ST_OPENCONFIRM) {
        state = ST_ESTABLISHED;
        established_at = now_ms();
        restarted = false;
        if (hold_time > 0)
          timer_keepalive = uloop.timer(hold_time * 1000 / 3, cb_keepalive);
        INFO('BGP session to bird established');
        if (opts.on_up)
          opts.on_up();
      }
      break;
    default:
      DBG('Ignoring BGP message of type %d', type);
  }
}

// uloop runs timers before it looks at the sockets. After we were blocked or
// not scheduled for long, the KEEPALIVEs of bird wait in the socket while the
// hold timer is already due: give them one chance to be read.
function cb_hold() {
  if (!hold_grace_used) {
    hold_grace_used = true;
    timer_hold.set(HOLD_GRACE_MS);
    return;
  }
  down('hold timer expired');
}

function cb_readable() {
  let chunk = sock.recv(65536);
  if (chunk == null)
    return down(sprintf('receive failed: %s', sock.error()));
  if (chunk == '')
    return down('closed by bird');

  let buf = rx + chunk;
  let o = 0;
  let len = length(buf);
  while (len - o >= HEADER_LEN) {
    let mlen = r16(buf, o + 16);
    if (mlen < HEADER_LEN || mlen > MAX_MSG_LEN)
      return down(sprintf('bad message length %d', mlen));
    if (len - o < mlen)
      break;
    rx_message(buf, o, o + mlen);
    // the message may have closed the session
    if (!sock)
      return;
    o += mlen;
  }
  rx = substr(buf, o);
}

connect = function() {
  if (state != ST_IDLE)
    return;
  DBG('Connecting to bird: %s port %d', opts.host, opts.port);
  let s = socket.connect(opts.host, opts.port, { socktype: socket.SOCK_STREAM }, 1000);
  if (!s) {
    DBG('Could not connect to bird: %s', socket.error());
    schedule_reconnect(opts.reconnect_interval);
    return;
  }
  sock = s;
  rx = '';
  state = ST_CONNECTED;
  hold_time = opts.hold_time;
  hold_grace_used = false;
  // bird sends its OPEN first, we answer with ours
  timer_hold = uloop.timer(opts.hold_time * 1000, cb_hold);
  handle = uloop.handle(sock, cb_readable, uloop.ULOOP_READ);
};

function established() {
  return state == ST_ESTABLISHED;
}

// Announce the host route of ip with data as attribute 250. Announcing an IP
// again replaces what bird has.
function announce(ip, data) {
  let addr = iptoarr(ip);
  if (!established() || !addr)
    return false;

  let attrs = attribute(0x40, ATTR_ORIGIN, chr(0))
            + attribute(0x40, ATTR_AS_PATH, '');
  let nlri = '';
  if (length(addr) == 4) {
    attrs += attribute(0x40, ATTR_NEXT_HOP, chr(...NEXT_HOP_V4));
    nlri = chr(32, ...addr);
  } else {
    attrs += attribute(0x80, ATTR_MP_REACH,
                       chr(0, 2, 1, 16, ...NEXT_HOP_V6) + chr(0) + chr(128, ...addr));
  }
  attrs += attribute(0x40, ATTR_LOCAL_PREF, u32(100))
         + attribute(0xc0, ATTR_BGPDISCO, data);

  let body = u16(0) + u16(length(attrs)) + attrs + nlri;
  if (HEADER_LEN + length(body) > MAX_MSG_LEN) {
    ERR('Data of %s does not fit into a BGP message (%d bytes) - not announced', ip, length(data));
    return false;
  }
  return send(MSG_UPDATE, body);
}

function withdraw(ip) {
  let addr = iptoarr(ip);
  if (!established() || !addr)
    return false;

  if (length(addr) == 4)
    return send(MSG_UPDATE, u16(5) + chr(32, ...addr) + u16(0));

  let attrs = attribute(0x80, ATTR_MP_UNREACH, chr(0, 2, 1) + chr(128, ...addr));
  return send(MSG_UPDATE, u16(0) + u16(length(attrs)) + attrs);
}

// Tells bird that it has all our routes. After a restart of bgpdisco, bird
// then drops what it kept from the previous session and did not get again.
function end_of_rib() {
  if (!established())
    return false;
  let attrs = attribute(0x80, ATTR_MP_UNREACH, chr(0, 2, 1));
  return send(MSG_UPDATE, u16(0) + u16(0))
      && send(MSG_UPDATE, u16(0) + u16(length(attrs)) + attrs);
}

function init(options) {
  for (let k, v in options)
    if (v != null)
      opts[k] = v;

  if (type(opts.router_id) == 'string') {
    let a = iptoarr(opts.router_id);
    let id = (length(a) == 4) ? ((a[0] << 24) | (a[1] << 16) | (a[2] << 8) | a[3]) : 0;
    // bird refuses 0.0.0.0
    if (id != 0) {
      opts.router_id = id;
    } else {
      ERR('Invalid router ID "%s" - deriving it from the router ID of bird', opts.router_id);
      opts.router_id = null;
    }
  }

  if (opts.graceful_restart_time < 0 || opts.graceful_restart_time > MAX_GRACEFUL_RESTART_TIME) {
    let t = (opts.graceful_restart_time < 0) ? 0 : MAX_GRACEFUL_RESTART_TIME;
    WARN('Graceful restart time %d is out of range - using %d', opts.graceful_restart_time, t);
    opts.graceful_restart_time = t;
  }

  return {
    start: connect,
    established,
    announce,
    withdraw,
    end_of_rib,
  };
}

export { init };
