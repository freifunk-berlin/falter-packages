'use strict';
'require view';
'require form';

return view.extend({
  render: function () {
    var m, s, t, o1, o2, o3, o4, o5;

    addNotification('Warnasdfads', )

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

    o1 = s.option(form.Value, 'selector_fqdn', _('Selector-FQDN'),
      _('URL of the firmware-selector without protocol. Usally: selector.berlin.freifunk.net'));
    o1.datatype = 'hostname';

    o2 = s.option(form.Value, 'fw_server_fqdn', _('Firmware-Server-FQDN'),
      _('URL of the firmware server without protocol. Usally firmware.berlin.freifunk.net'));
    o2.datatype = 'hostname';

    o3 = s.option(form.Value, 'minimum_certs', _('Minimum Certs'),
      _('Minimum amount of certificates that must be valid. Otherwise autoupdate will not perform an upgrade.'));
    o3.datatype = 'uinteger';

    o4 = s.option(form.Flag, 'disabled', _('Disabled'),
      _('Deactivates the Autoupdater. We do not recommend this!'));
    o4.rmempty = false;

    o5 = s.option(form.Flag, 'ignore_mod', _('Ignore config changes'),
    _("The autoupdater will detect custom changes you've applied to your router since you've run the wizard.\
       It will refuse an autoupdate then, to avoid breaking your customized setup. If you wish to automatically\
       update anyway, activate this option. We do not recommend this, as this might break your setup on auto-updates."));


    // t = m.section(form.TypedSection, 'autoupdatehints', _())

    return m.render();
  }
});
