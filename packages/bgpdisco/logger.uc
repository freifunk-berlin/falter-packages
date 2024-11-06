import {fdopen} from 'fs';
import {traceback} from 'debug';
import * as log from 'log';

let tty;
let debug = false;

function _infos_from_stacktrace() {
  let st = traceback(4)[0];
  let _fn = split(st.filename, '/');
  return {
   filename: pop(_fn)
  };
}

function _log(priority, ...args) {
  let info = _infos_from_stacktrace();
  let msg = sprintf(...args);

  if (tty) {
    let t = localtime();
    let time_str = sprintf('%02d:%02d:%02d', t.hour, t.min, t.sec);
    let fmt = '%s [%s] %s: %s\n';
    printf(fmt, time_str, priority, info.filename, msg);

  } else {
    if (info.filename == 'bgpdisco')
      info.filename = 'main';
    log.syslog(priority, '%s: %s', info.filename, msg);
 }
}

function DBG(...args) {
  if (!debug)
    return;
  _log('debug', ...args);
}

function INFO(...args) {
  _log('info', ...args);
}

function WARN(...args) {
  _log('warn', ...args);
}

function ERR(...args) {
  _log('err', ...args);
}

function enable_debug() {
  INFO('Enable Debugging');
  debug = true;
}

tty = fdopen(0, 'r').isatty();


if (!tty)
  log.openlog('bgpdisco', log.LOG_PID, log.LOG_DAEMON);

export { enable_debug, DBG, INFO, WARN, ERR };
