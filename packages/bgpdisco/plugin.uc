import { lsdir } from 'fs';
import { DBG, INFO, WARN, ERR } from 'bgpdisco.logger';

const CB_TYPE = {
  DATA_PROVIDER: 0,
  DATA_HANDLER: 1
};

let cb_providers = [];
let cb_handlers = [];


function provide_data() {
  DBG('Requesting data from all registered plugins');
  let data = {};
  for (let plugin in cb_providers) {
      let id = plugin[0];
      let cb = plugin[1];

      let res_cb = call(cb);
      for (let _ip in res_cb) {
        let res_cb_data = res_cb[_ip];
        data[_ip] ??= [];

        let darr = [];
        if(type(res_cb_data == 'array')) {
          darr = res_cb_data;
          unshift(res_cb_data, id); // insert id before data elements
        } else {
          push(darr, id, res_cb_data);
        }
        push(data[_ip], darr);
      }
  }
  return data;
}

function handle_data(data) {
  let leftover_ids = map(keys(data), (x) => int(x));
  DBG('Handling incoming data for IDs: %s', leftover_ids);
  for (let plugin in cb_handlers) {
      let id = plugin[0];
      let cb = plugin[1];
      call(cb,null,null,data[id]);
      leftover_ids = filter(leftover_ids, (i) => i != id); // remove id from list
  }
  if (length(leftover_ids) > 0)
      WARN('Received data without a registered handler: %s', leftover_ids);
}

export function init(plugin_dir) {
  DBG('init()');
  let cb_register = function (type, id, cb) {
    if (type == CB_TYPE.DATA_PROVIDER) {
      INFO('Registering plugin as provider for ID %d', id);
      push(cb_providers, [id, cb]);
    } else if (type == CB_TYPE.DATA_HANDLER)  {
      INFO('Registering plugin as handler for ID %d', id);
      push(cb_handlers, [id, cb]);
    }
  };

  for (let plugin_file in lsdir(plugin_dir)) {
    let mod = loadfile(plugin_dir + '/' + plugin_file);
    let mod_entry = mod();
    INFO('Initializing plugin %s', plugin_file);
    mod_entry.init({register: cb_register, TYPE: CB_TYPE});
  }

  return {
    provide_data: provide_data,
    handle_data: handle_data
  };
};
