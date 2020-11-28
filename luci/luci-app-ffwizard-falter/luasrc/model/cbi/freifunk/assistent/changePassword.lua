local uci = require "luci.model.uci".cursor()

f = SimpleForm("ffwizward", "", "")
--change button texts
f.submit = "Next"
--hide reset button
f.reset = false
--enable skip button
--f.flow = {}
--f.flow["skip"] = true

css = f:field(DummyValue, "css", "")
css.template = "freifunk/assistent/snippets/css"

welcome = f:field(DummyValue, "welcome", "")
welcome.template = "freifunk/assistent/snippets/welcome" 

pw1 = f:field(Value, "pw1", translate("Password"))
pw1.password = true
function pw1.validate(self, value, section)
  return pw2:formvalue(section) == value and value
end

pw2 = f:field(Value, "pw2", translate("Confirmation"))
pw2.password = true
function pw2.validate(self, value, section)
  return pw1:formvalue(section) == value and value
end

function f.handle(self, state, data)
  if state == FORM_SKIP then
    --this never happens :(
  end
  if state == FORM_VALID then
    if data.pw1 and data.pw2 then
      local stat = luci.sys.user.setpasswd("root", data.pw1) == 0
    end
    data.pw1 = nil
    data.pw2 = nil
    luci.http.redirect(luci.dispatcher.build_url("admin/freifunk/assistent/generalInfo"))
  end
end

function f.on_cancel()
  luci.http.redirect(luci.dispatcher.build_url("admin/freifunk/assistent/cancel"))
end

return f
