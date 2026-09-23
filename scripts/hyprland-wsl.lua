local path = os.getenv("OMARCHY_PATH") or "/home/omarchy/.local/share/omarchy"
dofile(path .. "/default/hypr/bootstrap.lua")

require("default.hypr.helpers")
local require_optional = require("default.hypr.require_optional")
if _G.omarchy_default_bindings ~= false then
  require("default.hypr.bindings.media")
  require("default.hypr.bindings.clipboard")
  require("default.hypr.bindings.tiling")
  require("default.hypr.bindings.utilities")
  require("default.hypr.bindings.voxtype")
  require_optional.module("default.hypr.bindings.applications")
end
require("default.hypr.envs")
require("default.hypr.looknfeel")
require("default.hypr.input")
require("default.hypr.windows")
require_optional.module("omarchy.current.theme.hyprland")
require("hypr.monitors")
require("hypr.input")
require("hypr.bindings")
require("hypr.looknfeel")
require("hypr.autostart")
require("default.hypr.toggles")

hl.on("hyprland.start", function()
  hl.exec_cmd("omarchy-wsl-session-init")
end)
