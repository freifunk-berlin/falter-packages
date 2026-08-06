'use strict';

const ubus = require('ubus').connect();

const methods = {
  status: {
    args: {},
    call: (req) => {
      return {
        status: 'hello',
      };
    },
  },
};

return { falter: methods };
