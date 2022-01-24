'use strict';
'require view';
'require form';

return view.extend({
  render: function () {
    var m, s, o;

    m = new form.Map('autoupdate', _('Freifunk Berlin Autoupdate'),
      _("Autoupdate will update your router automatically, once there is a new\
         firmware version available.\
         <br><br>Hence we do our best in testing upgrades \
         extensively, there might be a small chance of failing upgrades. For example this \
         could happen if you costumized your settings a lot. We always recommend you\
         to keep a backup of your latest working router configuration.\
         <br><br>\
         To obtain a backup of your current settings, go to <i> System -&gt Backup</i> and\
         download the backup-archive."));

    s = m.section(form.TypedSection, 'generic', _('Settings'));
    s.anonymous = true;

    s.option(form.Value, 'selector_fqdn', _('Selector-FQDN'),
      _('Selector-URL witout protocol, i.e.: selector.berlin.freifunk.net'));

    s.option(form.Value, 'minimum_certs', _('Minimum Certs'),
      _('Minimum amount of certificates that must be valid. Otherwise autoupdate will not perform an upgrade.'));

    o = s.option(form.Flag, 'disabled', _('Disabled'),
      _('Deactivates the Autoupdater. We do not recommend this!'));
    o.rmempty = false;

    return m.render();
  }
});
