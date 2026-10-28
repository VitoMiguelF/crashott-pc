-- Profile mp_tres_crash (config/run.toml, docs/internals/multiplayer.md): a third Crash in the
-- village, spawned the way the level spawns players 1 and 2 (objectives.lua: DO_SpawnPlayer).
@gameplay_ready+2
dev.objective("mp_spawn_p3", function(o) o:DO_SpawnPlayer(3, "ref_Crash_P3", "PlayerSpawnPosition", "") end)
@gameplay_ready+4
local streamer = objectManager:lookupObject(":Streamer")
for _, name in ipairs({"ref_Crash", "ref_Crash_P2", "ref_Crash_P3"}) do
  local a = streamer:GetActorByName(name)
  if exists(a) then
    local v = a:GetTransformRow(3)
    devprint("mp actor " .. name .. " at " .. v.x .. " " .. v.y .. " " .. v.z)
  else
    devprint("mp actor " .. name .. " missing")
  end
end
@gameplay_ready+12
local a = objectManager:lookupObject(":Streamer"):GetActorByName("ref_Crash_P3")
if exists(a) then local v = a:GetTransformRow(3) devprint("mp actor ref_Crash_P3 later at " .. v.x .. " " .. v.y .. " " .. v.z) end
dev.pos(0)
