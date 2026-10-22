-- Loads the game's own GameLink helpers (script/gamelinkutil.lua: Radical's remote debugger,
-- skipped by init.lua in the retail game because RAD_RELEASE is true) and adds dev.* helpers.
-- Prepend it to a dev Lua file, or run it as the first block (globals stay for later blocks).
-- The game's Lua is 5.0-flavoured: no "#" operator and no "..." in expressions.
if type(GameLinkUtil) ~= "table" then
  local saved = RAD_RELEASE
  RAD_RELEASE = false
  require("script/gamelinkutil.lua")
  RAD_RELEASE = saved
end

dev = dev or {}

-- One line per value, nested tables flattened as a.b.c (depth-limited).
function dev.show(label, v, depth)
  depth = depth or 0
  if type(v) == "table" and depth < 4 then
    local empty = true
    for k, x in pairs(v) do
      empty = false
      dev.show(label .. "." .. tostring(k), x, depth + 1)
    end
    if empty then devprint(label .. " = {}") end
  else
    devprint(label .. " = " .. type(v) .. " " .. tostring(v))
  end
end

-- Player 1's actor (the CActor the camera follows).
function dev.player(index)
  return Managers.GameDatabase:GetCurrentPlayer(index or 0)
end

-- Mojo: +amount to the current episode's count (the HUD counter follows).
function dev.give_mojo(amount)
  Managers.StatsManager:IncrementValueInt(EStatInt_MojoCount, amount)
  return Managers.StatsManager:GetValueInt(EStatClass_Current, EStatInt_MojoCount)
end
