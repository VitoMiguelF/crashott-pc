-- Profile mp_ko (config/run.toml, docs/internals/multiplayer.md): players 3 and 2 are knocked out
-- (TakeDamage past their hitpoints) to see the co-op knock-out / respawn with 4 players.
@gameplay_ready+6
local a = objectManager:lookupObject(":Streamer"):GetActorByName("ref_Crash_P3")
if a ~= nil then devprint("mp_ko P3") a.m_BehaviourController.m_DamageableBehaviour:TakeDamage(1000) end
@gameplay_ready+12
local a = objectManager:lookupObject(":Streamer"):GetActorByName("ref_Crash_P2")
if a ~= nil then devprint("mp_ko P2") a.m_BehaviourController.m_DamageableBehaviour:TakeDamage(1000) end
@gameplay_ready+20
for _, name in ipairs({"ref_Crash", "ref_Crash_P2", "ref_Crash_P3", "ref_Crash_P4"}) do
  local a = objectManager:lookupObject(":Streamer"):GetActorByName(name)
  if a ~= nil then local v = a:GetTransformRow(3) devprint("mp_ko " .. name .. " at " .. v.x .. " " .. v.y .. " " .. v.z) end
end
