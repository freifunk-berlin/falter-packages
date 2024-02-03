'use strict';
'require view';
'require form';

return view.extend({
  render: function () {
    var m, s, t, u, o;
    var opt1, opt2, opt3, opt4, opt5, opt6, opt7, opt8, opt9;

    m = new form.Map('ffwizard3', _('Freifunk Wizard'),
      _("Herzlichen Glueckwunsch! \
      <br/>Du hast deinen Router erfolgreich mit der Freifunk-Firmware ausgestattet. \
      Dieser Assistent hilft dir beim Einrichten deines Routers. Wenn du keine Hilfe \
      brauchst, kannst du in der Administrationsoberfläche \
      die Einstellungen auch alleine vornehmen. \
      <br/>Aendere hier zunaechst dein Password. Bitte waehle ein \
      sicheres Passwort, da der Router nach aussen sichtbar ist. \
      Spaeter findest du diese Option im Bereich <em>System &gt; Administration.</em> \
      <br><br> \
      <b>Bitte beachte:</b> Um diesen Wizard benutzen zu können, musst du vorher IP-Adressen \
      auf https://config.berlin.freifunk.net/ beantragt haben. Falls du das noch nicht \
      getan hast, hole das am besten gleich nach. :)\
      "));
    m.tabbed = true;

    s = m.section(form.TypedSection, 'contact', _('Contact and Access'));
    s.anonymous = true;

    t = m.section(form.TypedSection, 'router_config', _('Router Configuration'));
    // add text explaining the section
    t.anonymous = true;


    s.option(form.Value, 'nickname', _('Nickname'));
    s.option(form.Value, 'realname', _('Real Name'));
    s.option(form.Value, 'email', _('E-Mail'));
    s.option(form.Value, 'contact_url', _('Contact URL'));
    s.option(form.Value, 'password', _('Password'));
    s.option(form.Value, 'repeatpassword', _('Repeat Password'));

    opt1 = t.option(form.ListValue, 'community', _('Chose a Community'));
    opt1.value('berlin', 'Freifunk Berlin');
    opt1.value('fuerstenwalde', 'Freifunk Fürstenwalde');
    opt1.rmempty = false;
    opt1.editable = true;

    t.option(form.Value, 'nodename', _('Node Name'));
    t.option(form.Value, 'location', _('Location'));

    // Show clickable Map

    t.option(form.Value, 'lat', _('Latitude'));
    t.option(form.Value, 'lon', _('Longitude'));

    // Button: Autodetect from Browser (location)

    opt2 = t.option(form.ListValue, 'share_internet', _('Share Internet'));
    opt2.value('tunneldigger', _('Yes, via Tunneldigger tunnel'));
    opt2.value('wireguard', _('Yes, via Wireguard tunnel'));
    opt2.value('direct', _('Yes, direct connection'));
    opt2.value('false', _('No'));
    opt2.rmempty = false;
    opt2.editable = true;

    // ToDo: Only show this, if meshing wifi+internet was selected
    opt3 = t.option(form.ListValue, 'mesh_via', _('Mesh via'));
    opt3.value('wifi+internet', _('WiFi and Internet'));
    opt3.value('wifi_only', _('Wifi only'));
    opt3.rmempty = false;
    opt3.editable = true;

    t.option(form.Value, 'download_bandwidth', _('Download-Bandwidth'));
    t.option(form.Value, 'upload_bandwidth', _('Upload-Bandwidth'));

    // ToDo: Bandwidth measurement

    opt4 = t.option(form.ListValue, 'auto_bandwidth', _('Auto bandwidth'));
    opt4.value('dsl_25_5', 'DSL 25/5');
    opt4.value('dsl_50_10', 'DSL 50/10');
    opt4.value('dsl_100_25', 'DSL 100/25');
    opt4.rmempty = false;
    opt4.editable = true;

    // ToDo: heading: "Monitoring & Updates"

    opt5 = t.option(form.ListValue, 'monitoring', _('Enable Monitoring'));
    opt5.value('true', _('Yes'));
    opt5.value('false', _('No'));
    opt5.rmempty = false;
    opt5.editable = true;

    opt6 = t.option(form.ListValue, 'autoupdate', _('Auto Update'));
    opt6.value('stable', _('Yes (stable)'));
    opt6.value('development', _('Yes (development)'));
    opt6.value('false', _('Disabled'));
    opt6.rmempty = false;
    opt6.editable = true;

    // ToDo: heading "IP Configuration"
    opt7 = t.option(form.DynamicList, 'ipv4_mesh_ips', _('IPv4 Mesh IPs'));
    // ToDo: validate as addresses/nets
    opt7.rmempty = false;
    opt7.editable = true;

    opt8 = t.option(form.DynamicList, 'ipv4_dhcp', _('IPv4 DHCP-Network'));
    // ToDo: validate as addresses/nets
    opt8.rmempty = false;
    opt8.editable = true;

    opt9 = t.option(form.DynamicList, 'ipv6_ips', _('IPv6 IPs'));
    // ToDo: validate as addresses/nets
    opt9.rmempty = false;
    opt9.editable = true;

    // o = s.option(form.Flag, 'disabled', _('Disabled'),
    //   _('Deactivates the Autoupdater. We do not recommend this!'));
    // o.rmempty = false;

    return m.render();
  }
});
