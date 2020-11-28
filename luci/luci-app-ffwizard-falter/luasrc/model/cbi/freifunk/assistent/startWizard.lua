local uci = require "luci.model.uci".cursor()
local fs = require "nixio.fs"
local fftools = require "luci.tools.freifunk.assistent.tools"

-- check to see if the pw is already set
if uci:get("ffwizard","settings","runbefore") and fftools.hasRootPass() then

  -- check for any upgrades
  local lastUpgrade = uci:get("ffwizard","upgrade","lastUpgrade") or ""
  local nextUpgrade = false
  local upgradesDir = "/usr/lib/lua/luci/model/cbi/freifunk/upgrades/"
  for upgrade in fs.glob(upgradesDir.."*") do

    local upgradeName = string.gsub(upgrade,upgradesDir,"")                               
    upgradeName = string.gsub(upgradeName,".lua","")                                      

    if nextUpgrade or lastUpgrade == "" then
      luci.http.redirect(luci.dispatcher.build_url("admin/freifunk/upgrades/"..upgradeName))
    end

    if upgradeName == lastUpgrade then
      nextUpgrade = true
    end
  end

  -- go to the status page, no wizard is needed
  luci.http.redirect(luci.dispatcher.build_url("admin/status/overview"))
end

-- Start up the firstboot wizard
luci.http.redirect(luci.dispatcher.build_url("admin/freifunk/assistent/changePassword"))
