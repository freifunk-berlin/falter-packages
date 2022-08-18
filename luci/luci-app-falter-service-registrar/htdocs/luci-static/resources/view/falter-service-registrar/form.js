'use strict';
'require view';
'require form';
'require validation';

return view.extend({
    render: function () {

        function olsr_description_validator(section_id, value) {
            return (String(value).match(/^[a-zA-Z0-9 \-]+$/) && String(value).length <= 75 ? true : _('Require string with characters, numbers and whitespace only. It must be less than 75 characters long.'))
        };

        var m, s, o, t, opt;

        m = new form.Map('ffservices', _('Set up Freifunk services'), _(`<br>
        <p>This site gives you the chance to define your own services for the Freifunk network. If you enter the service
        details here, they will get announced to the mesh and other Freifunkers can use them. It's quite similar
        to regular web services, except this ones only live in Freifunk network. But there are other ways to get
        your service exposed to the internet. Just ask us, if you are interested in that.</p>

        <p>You can find two tabs in this app: <b>Websites</b> and <b>Generic Services</b>.
        They differ in what you want to do and how much you are experienced.</p>

        <p><b>Websites</b> will configure you a (static) website on your router. It will not only announce it to the mesh, but
        will additionally set up OpenWrts <i>uhttpd</i>-webserver, to serve your site. Please mind: As by the limitations
        of <i>uhttpd</i>, every Website must have it's own port. Especially you cannot use port 80, unless you deactivate
        the LuCI web-ui, which uses that port per default.</p>

        <p>The <b>Generic Services</b> gives you more flexibility in that way, that you can configure services that run on
        some other host in your local Freifunk net. If you are going to expose a webcam or some kind of server, this
        option is the right one for you.</p><br>`));
        m.tabbed = true;

        s = m.section(form.TypedSection, 'website', _('Websites'));
        s.addremove = true;
        s.anonymous = true;

        t = m.section(form.TypedSection, 'service', _('Generic Services'));
        t.addremove = true;
        t.anonymous = true;

        var o0, o1, o2, o3, o4, o5;
        o0 = s.option(form.Value, 'fqdn', _('FQDN'), _('The Fully-Qualified-Domain-Name of your website, i.e.: "example.olsr". This is URL on which your service will be available in the Freifunk network.'));
        o0.rmempty = false;
        o0.datatype = 'hostname';
        o1 = s.option(form.Value, 'description', _('Description'), _('Give a short description or title'));
        o1.rmempty = false;
        o1.validate = olsr_description_validator;

        o2 = s.option(form.ListValue, 'protocol', _('Protocol'));
        o2.value('tcp', 'TCP');
        o2.rmempty = false;
        o2.editable = false;

        o3 = s.option(form.Value, 'port', _('Port'));
        o3.datatype = 'and(port, min(81))';
        o3.rmempty = false;
        o4 = s.option(form.Value, 'web_root', _('Web Root'), _('Give the directory where your site lives.'));
        o4.rmempty = false;
        o4.datatype = 'and(directory, maxlength(253))';
        o5 = s.option(form.Flag, 'disabled', _('Disabled'), _("Don't publish this site"));
        o5.default = '0';
        o5.rmempty = false;

        var opt0, opt1, opt2, opt3, opt4, opt5;
        opt0 = t.option(form.Value, 'fqdn', _('FQDN'), _('The FQDN of your website, i.e.: "example.olsr"'));
        opt0.rmempty = false;
        opt0.datatype = 'hostname';
        opt1 = t.option(form.Value, 'description', _('Description'), _('Give a short description or title'));
        opt1.rmempty = false;
        opt1.validate = olsr_description_validator;

        opt2 = t.option(form.ListValue, 'protocol', _('Protocol'));
        opt2.value('tcp', 'TCP');
        opt2.value('udp', 'UDP');
        opt2.rmempty = false;
        opt2.editable = true;

        opt3 = t.option(form.Value, 'port', _('Port'));
        opt3.datatype = 'and(port, min(80))';
        opt3.rmempty = false;
        opt4 = t.option(form.Value, 'ip_addr', _('IP-Address'), _('Must be a valid address from the routers host network.'));
        opt4.datatype = 'ipaddr';
        opt4.rmempty = false;
        opt5 = t.option(form.Flag, 'disabled', _('Disabled'), _("Don't publish this service"));
        opt5.default = '0';
        opt5.rmempty = false;

        return m.render();
    },
});
