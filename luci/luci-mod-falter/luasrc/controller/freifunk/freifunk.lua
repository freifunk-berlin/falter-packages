-- Copyright 2008 Steven Barth <steven@midlink.org>
-- Licensed to the public under the Apache License 2.0.

module("luci.controller.freifunk.freifunk", package.seeall)

function index()
	local page

	-- Frontend
	page          = node()
	page.lock     = true
	page.subindex = true
	page.index    = false

	page          = node("freifunk")
	page.title    = _("Freifunk")
	page.target   = template("freifunk/index")
	page.order    = 5
	page.setuser  = "nobody"
	page.setgroup = "nogroup"
	page.i18n     = "freifunk"
	page.index    = true

	page          = node("freifunk", "index")
	page.target   = template("freifunk/index")
	page.title    = _("Overview")
	page.order    = 10
	page.indexignore = true

	page          = node("freifunk", "contact")
	page.target   = template("freifunk/contact")
	page.title    = _("Contact")
	page.order    = 15

	page          = node("freifunk", "status")
	page.target   = template("freifunk/public_status")
	page.title    = _("Status")
	page.order    = 20
	page.i18n     = "base"
	page.setuser  = false
	page.setgroup = false

	if nixio.fs.access("/usr/sbin/luci-splash") then
		assign({"freifunk", "status", "splash"}, {"splash", "publicstatus"}, _("Splash"), 40)
	end

	page = assign({"freifunk", "olsr"}, {"admin", "status", "olsr"}, _("OLSR"), 30)
	page.setuser = false
	page.setgroup = false
	page.acl_depends = false

	if nixio.fs.access("/etc/config/luci_statistics") then
		assign({"freifunk", "graph"}, {"admin", "statistics", "graph"}, _("Statistics"), 40)
	end

	-- backend
	assign({"mini", "freifunk"}, {"admin", "freifunk"}, _("Freifunk"), 5)

	page        = node("admin", "freifunk")
	page.target = template("freifunk/adminindex")
	page.title  = _("Freifunk")
	page.order  = 5

	page        = node("admin", "freifunk", "basics")
	page.target = cbi("freifunk/basics")
	page.title  = _("Basic Settings")
	page.order  = 5
	
	page        = node("admin", "freifunk", "basics", "profile")
	page.target = cbi("freifunk/profile")
	page.title  = _("Profile")
	page.order  = 10

	page        = node("admin", "freifunk", "basics", "profile_expert")
	page.target = cbi("freifunk/profile_expert")
	page.title  = _("Profile (Expert)")
	page.order  = 20

	page        = node("admin", "freifunk", "contact")
	page.target = cbi("freifunk/contact")
	page.title  = _("Contact")
	page.order  = 15

	page        = node("admin", "freifunk", "Index-Page")
	page.target = cbi("freifunk/user_index")
	page.title  = _("Index Page")
	page.order  = 25

	page        = node("admin", "freifunk", "wireless-mesh")
	page.target = cbi("freifunk/wireless-mesh", {hideapplybtn=true})
	page.title  = _("Wireless Mesh")
	page.order  = 30

        page        = node("admin", "freifunk", "bandwidth")
        page.target = cbi("freifunk/bandwidth", {hideapplybtn=true})
        page.title  = _("Bandwidth")
        page.order  = 35

	page        = node("admin", "freifunk", "bbbdigger")
	page.target = cbi("freifunk/bbbdigger", {hideapplybtn=true})
	page.title  = _("BBB-VPN (bbbdigger)")
	page.order  = 40

	page        = node("admin", "freifunk", "swapports")
	page.target = cbi("freifunk/swapports", {hideapplybtn=true})
	page.title  = _("Swap Physical Ports")
        page.order  = 41

	entry({"freifunk", "map"}, template("freifunk-map/frame"), _("Map"), 50)
	entry({"freifunk", "map", "content"}, template("freifunk-map/map"), nil, 51)
	entry({"admin", "freifunk", "profile_error"}, template("freifunk/profile_error"))
end
