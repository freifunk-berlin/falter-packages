{%

'use strict';

import dispatch from 'luci.dispatcher';
import request from 'luci.http';

global.handle_request = function(env) {
  let req = request(env, uhttpd.read, uhttpd.write);
  dispatch(req);
  req.close();
};
