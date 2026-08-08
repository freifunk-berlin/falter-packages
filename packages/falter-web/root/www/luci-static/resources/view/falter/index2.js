'use strict';
'require rpc';
'require view';

const callFalterStatus = rpc.declare({
  object: 'falter',
  method: 'status',
  expect: { '': {} }
});

// We want to use our own user flow for setting the initial root password,
// but there seems to be no elegant way to detect password absence.
//
// So we look at the warning messages rendered by the header.ut template.
// We could match on the string "password", but virtually all translations
// do translate this word to something else. The other possible warning message
// always contains the string "initramfs" though, so we can match on that.
//
// We assume password absence if there's any warning that's not about initramfs.
function isPasswordAbsent() {
  const warnings = document
    .querySelectorAll('#maincontent>.warning h4')
    .values();
  return !!Array.from(warnings)
    .find((e) => !e.innerHTML.includes('initramfs'));
}

return view.extend({
  passwordAbsent: isPasswordAbsent(),

  load() {
    return L.resolveDefault(callFalterStatus());
  },

  render(falterStatus) {
    return E([], [
      E('h1', {}, _('Hello, world!')),
      E('pre', {}, `status: ${falterStatus.status}`),
      E('pre', {}, `passwordAbsent: ${(this.passwordAbsent)}`)
    ]);
  },

  handleReset: null,
  handleSave: null,
  handleSaveApply: null,

});
