-- Profile mp_tres_crash (config/run.toml, docs/internals/multiplayer.md): where the 4 players are
-- (CRASH_MP_PLAYERS=4 spawns players 3 and 4, mp_players.cpp).
@gameplay_ready+4
local streamer = objectManager:lookupObject(":Streamer")
for _, name in ipairs({"ref_Crash", "ref_Crash_P2", "ref_Crash_P3", "ref_Crash_P4"}) do
  local a = streamer:GetActorByName(name)
  if a ~= nil and a ~= NULL_OBJECT and a.GetTransformRow then
    local v = a:GetTransformRow(3)
    devprint("mp actor " .. name .. " at " .. v.x .. " " .. v.y .. " " .. v.z)
  else
    devprint("mp actor " .. name .. " missing")
  end
end
@gameplay_ready+12
local a = objectManager:lookupObject(":Streamer"):GetActorByName("ref_Crash_P3")
if a ~= nil and a ~= NULL_OBJECT and a.GetTransformRow then local v = a:GetTransformRow(3) devprint("mp actor ref_Crash_P3 later at " .. v.x .. " " .. v.y .. " " .. v.z) end
dev.pos(0)
