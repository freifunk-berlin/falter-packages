/*
'use strict';
'require view';
'require form';

return view.extend({
  render: function () {
    var m, s;

    m = new form.Map('ffwizard3', _('Contact and Login Data'));

    s = m.section(form.TypedSection, 'generic', _(''));
    s.anonymous = true;

    s.option(form.Value, 'nickname', _('Nickname'));
    s.option(form.Value, 'realname', _('Real Name'));
    s.option(form.Value, 'email', _('E-Mail'));
    s.option(form.Value, 'contacturl', _('Contact-URL'),
      _("URL to a contact-webform on config.berlin.freifunk.net. This is in case \
      you don't want to give your e-mail address."));

    s.option(form.Value, 'pwd', _('Password'));
    s.option(form.Value, 'pwdrepeat', _('Repeat Password'));

    return m.render();
  }
});
*/
