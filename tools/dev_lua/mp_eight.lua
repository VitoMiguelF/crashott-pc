-- Profile mp_oito_crash (config/run.toml, docs/internals/multiplayer.md "8 players"): where the 8 players are
-- (CRASH_MP_PLAYERS=8 spawns players 3-8, mp_players.cpp; 5-8 on controllers 4-7 of ours).
@gameplay_ready+5
local streamer = objectManager:lookupObject(":Streamer")
for _, name in ipairs({"ref_Crash", "ref_Crash_P2", "ref_Crash_P3", "ref_Crash_P4", "ref_Crash_P5", "ref_Crash_P6", "ref_Crash_P7", "ref_Crash_P8"}) do
  local a = streamer:GetActorByName(name)
  if a ~= nil and a ~= NULL_OBJECT and a.GetTransformRow then
    local v = a:GetTransformRow(3)
    devprint("mp actor " .. name .. " at " .. v.x .. " " .. v.y .. " " .. v.z)
  else
    devprint("mp actor " .. name .. " missing")
  end
end
@gameplay_ready+13
local streamer = objectManager:lookupObject(":Streamer")
for _, name in ipairs({"ref_Crash_P5", "ref_Crash_P6", "ref_Crash_P7", "ref_Crash_P8"}) do
  local a = streamer:GetActorByName(name)
  if a ~= nil and a ~= NULL_OBJECT and a.GetTransformRow then local v = a:GetTransformRow(3) devprint("mp actor " .. name .. " later at " .. v.x .. " " .. v.y .. " " .. v.z) end
end
