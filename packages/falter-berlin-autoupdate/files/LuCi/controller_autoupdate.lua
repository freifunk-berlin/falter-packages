module("luci.controller.admin.autoupdate", package.seeall)

function index()
	entry({"admin", "system", "autoupdate"}, cbi("autoupdate/autoupdate"), "Autoupdater", 8).dependent=false
end
