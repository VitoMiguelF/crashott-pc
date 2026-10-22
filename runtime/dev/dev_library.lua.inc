// The dev.* Lua helpers, run once in the game's state before the first dev block (dev_tools.cpp).
// The game's Lua is 5.0-flavoured: no "#" length operator, no "..." in expressions.
R"lua(
dev = dev or {}

-- The game's GameLink helpers (script/gamelinkutil.lua, Radical's remote debugger), skipped by
-- init.lua in the retail game: GameLinkUtil.GetObjectChildren, GetFieldList, DumpValues, ...
function dev.gamelink()
  if type(GameLinkUtil) ~= "table" then
    local saved = RAD_RELEASE
    RAD_RELEASE = false
    require("script/gamelinkutil.lua")
    RAD_RELEASE = saved
  end
  return GameLinkUtil
end

-- One log line per value; tables flattened as a.b.c (depth-limited).
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

function dev.player(index) return Managers.GameDatabase:GetCurrentPlayer(index or 0) end
function dev.controller(index) return dev.player(index).m_BehaviourController end

-- Position (x, y, z; y is up) of a player, also logged as "dev.pos x y z".
function dev.pos(index)
  local v = dev.player(index):GetTransformRow(3)
  devprint("dev.pos " .. v.x .. " " .. v.y .. " " .. v.z)
  return v
end

function dev.teleport(x, y, z, index)
  dev.controller(index).m_PhysicsBehaviour:Teleport(tovector(x, y, z))
  return dev.pos(index)
end

function dev.damage(amount, index)
  dev.controller(index).m_DamageableBehaviour:TakeDamage(amount, dev.player(index))
end
function dev.set_hitpoints(value, index) dev.controller(index).m_DamageableBehaviour:SetCurrentHitpoints(value) end
function dev.is_dead(index) return dev.controller(index).m_DamageableBehaviour:IsDead() end

function dev.mojo() return Managers.StatsManager:GetValueInt(EStatClass_Current, EStatInt_MojoCount) end
function dev.give_mojo(amount)
  Managers.StatsManager:IncrementValueInt(EStatInt_MojoCount, amount)
  devprint("dev.mojo " .. dev.mojo())
  return dev.mojo()
end

-- The CSettingsDatabase fields (m_CheatShadowCrash, m_SlowMotion, ...): read, or write then read.
function dev.setting(name, value)
  local s = getobj(":SettingsDatabase")
  if value ~= nil then s[name] = value end
  devprint("dev.setting " .. name .. " = " .. tostring(s[name]))
  return s[name]
end

function dev.level()
  local u = getobj(":UberManager")
  return u:GetLevelName(u:GetLevelIndex(), false)
end

-- An objective built by build(o) with DO_* / WAIT_* steps (the scripts' own objective system).
function dev.objective(name, build)
  local o = NewObjective(name)
  build(o)
  o:Done()
  return o
end

-- One skin into the unlocked list (alone: see dev.unlock_all for several) (20 slots, 0x824A8218): the 20 the game unlocks itself are
-- Crash plus 17 enemy skins (jacking / defeating that enemy, objectives.lua) and Halloween (50%) /
-- Valentine (all gold idols); Cortex 1, Yuktopus 18, Uka Uka 19, Nina 20 and the unused Crash_Hulk 22
-- are in ESkin but nothing unlocks them, and they only fit while a slot is free.
-- Skins 18, 19, 20 have no name in the action's table (sub_82215548): the game crashes on them.
dev.unnamed_skins = { [18] = true, [19] = true, [20] = true }
function dev.unlock_skin(skin)
  if dev.unnamed_skins[skin] then error("skin " .. skin .. " has no name in the game: it would crash") end
  dev.objective("dev_unlock_skin", function(o) o:DO_UnlockSkin(skin) end)
end

dev.retail_skins = { 21, 10, 5, 3, 15, 11, 24, 2, 13, 0, 4, 12, 17, 16, 8, 7, 6, 14, 23, 9 }

-- What the game itself can unlock, by objectives of the game's own objective system: the 20 retail
-- skins, the levels and the moves/upgrades. `extra_skins` (e.g. {ESkin_Crash_Hulk}) go in first; the
-- list keeps 20, so the last retail ones then don't fit. Limits found the hard way: an objective of
-- ~200 steps never runs, 9 objectives made at once crash (sub_82200E60), concept art 20-24 / enemy
-- info 1 / skins 18-20 have no name in the actions' tables and crash.
function dev.unlock_all(extra_skins)
  local i
  local skins = {}
  if extra_skins then
    for i = 1, table.getn(extra_skins) do
      if not dev.unnamed_skins[extra_skins[i]] then table.insert(skins, extra_skins[i]) end
    end
  end
  for i = 1, table.getn(dev.retail_skins) do table.insert(skins, dev.retail_skins[i]) end
  dev.objective("dev_unlock_skins", function(o) for i = 1, table.getn(skins) do o:DO_UnlockSkin(skins[i]) end end)
  dev.objective("dev_unlock_levels", function(o) for i = 0, 19 do o:DO_UnlockLevel(i) end end)
  dev.objective("dev_unlock_moves", function(o) for i = 0, 26 do o:DO_UnlockMove(i) end end)
  devprint("dev.unlock_all queued")
end

)lua"
