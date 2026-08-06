'use strict';
'require rpc';
'require view';

const callFalterStatus = rpc.declare({
  object: 'falter',
  method: 'status',
  expect: { '': {} }
});

return view.extend({

  load() {
    return L.resolveDefault(callFalterStatus());
  },

  render(falterStatus) {
    return E([], [
      E('h1', {}, _('Hello, world!')),
      E('pre', {}, `status: ${falterStatus.status}`),
    ]);
  },

  handleReset: null,
  handleSave: null,
  handleSaveApply: null,

});
